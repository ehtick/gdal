/******************************************************************************
 *
 * Project:  Oracle Spatial Driver
 * Purpose:  Implementation of the OGROCILayer class.  This is layer semantics
 *           shared between table accessors and ExecuteSQL() result
 *           pseudo-layers.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_oci.h"
#include "cpl_conv.h"

/************************************************************************/
/*                           OGROCILayer()                               */
/************************************************************************/

OGROCILayer::OGROCILayer(OGROCIDataSource *poDSIn) : poDS(poDSIn)

{
    poFeatureDefn = nullptr;
    poStatement = nullptr;

    pszQueryStatement = nullptr;
    nResultOffset = 0;
    pszGeomName = nullptr;
    iGeomColumn = -1;
    pszFIDName = nullptr;
    iFIDColumn = -1;

    hLastGeom = nullptr;
    hLastGeomInd = nullptr;

    iNextShapeId = 0;
}

/************************************************************************/
/*                            ~OGROCILayer()                             */
/************************************************************************/

OGROCILayer::~OGROCILayer()

{
    if (m_nFeaturesRead > 0 && poFeatureDefn != nullptr)
    {
        CPLDebug("OCI", "%d features read on layer '%s'.", (int)m_nFeaturesRead,
                 poFeatureDefn->GetName());
    }

    OGROCILayer::ResetReading();

    CPLFree(pszGeomName);
    pszGeomName = nullptr;

    CPLFree(pszFIDName);
    pszFIDName = nullptr;

    CPLFree(pszQueryStatement);
    pszQueryStatement = nullptr;

    if (poFeatureDefn != nullptr)
        poFeatureDefn->Release();
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGROCILayer::ResetReading()

{
    if (poStatement != nullptr)
        delete poStatement;
    poStatement = nullptr;

    iNextShapeId = 0;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/*                                                                      */
/*      By default we implement the full spatial and attribute query    */
/*      semantics manually here.  The table query class will            */
/*      override this method and implement these inline, but the        */
/*      simple SELECT statement evaluator (OGROCISelectLayer) will      */
/*      depend us this code implementing additional spatial or          */
/*      attribute query semantics.                                      */
/************************************************************************/

OGRFeature *OGROCILayer::GetNextFeature()

{
    while (true)
    {
        OGRFeature *poFeature;

        poFeature = GetNextRawFeature();
        if (poFeature == nullptr)
            return nullptr;

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
            return poFeature;

        delete poFeature;
    }
}

/************************************************************************/
/*                         GetNextRawFeature()                          */
/************************************************************************/

OGRFeature *OGROCILayer::GetNextRawFeature()

{
    /* -------------------------------------------------------------------- */
    /*      Do we need to establish an initial query?                       */
    /* -------------------------------------------------------------------- */
    if (iNextShapeId == 0 && poStatement == nullptr)
    {
        if (!ExecuteQuery(pszQueryStatement))
            return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Have we run out of query results, such that we have no          */
    /*      statement left?                                                 */
    /* -------------------------------------------------------------------- */
    if (poStatement == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Are we in some sort of error condition?                         */
    /* -------------------------------------------------------------------- */
    hLastGeom = nullptr;

    char **papszResult = poStatement->SimpleFetchRow();

    if (papszResult == nullptr)
    {
        iNextShapeId = MAX(1, iNextShapeId);
        delete poStatement;
        poStatement = nullptr;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a feature from the current result.                       */
    /* -------------------------------------------------------------------- */
    int iField;
    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);

    poFeature->SetFID(iNextShapeId);
    iNextShapeId++;
    m_nFeaturesRead++;

    if (iFIDColumn != -1 && papszResult[iFIDColumn] != nullptr)
        poFeature->SetFID(atoi(papszResult[iFIDColumn]));

    for (iField = 0; iField < poFeatureDefn->GetFieldCount(); iField++)
    {
        if (papszResult[iField] != nullptr)
            poFeature->SetField(iField, papszResult[iField]);
        else
            poFeature->SetFieldNull(iField);
    }

    /* -------------------------------------------------------------------- */
    /*      Translate geometry if we have it.                               */
    /* -------------------------------------------------------------------- */
    if (iGeomColumn != -1)
    {
        poFeature->SetGeometryDirectly(TranslateGeometry());

        OGROCISession *poSession = poDS->GetSession();

        if (poFeature->GetGeometryRef() != nullptr && hLastGeom != nullptr)
            poSession->Failed(OCIObjectFree(poSession->hEnv, poSession->hError,
                                            static_cast<dvoid *>(hLastGeom),
                                            (ub2)OCI_OBJECTFREE_FORCE));

        hLastGeom = nullptr;
        hLastGeomInd = nullptr;
    }

    nResultOffset++;

    return poFeature;
}

/************************************************************************/
/*                            ExecuteQuery()                            */
/*                                                                      */
/*      This is invoke when the first request for a feature is          */
/*      made.  It executes the query, and binds columns as needed.      */
/*      The OGROCIStatement is used for most of the work.               */
/************************************************************************/

int OGROCILayer::ExecuteQuery(const char *pszReqQuery)

{
    OGROCISession *poSession = poDS->GetSession();

    CPLAssert(pszReqQuery != nullptr);
    CPLAssert(poStatement == nullptr);

    /* -------------------------------------------------------------------- */
    /*      Execute the query.                                              */
    /* -------------------------------------------------------------------- */
    poStatement = new OGROCIStatement(poSession);
    if (poStatement->Execute(pszReqQuery) != CE_None)
    {
        delete poStatement;
        poStatement = nullptr;
        return FALSE;
    }
    nResultOffset = 0;

    /* -------------------------------------------------------------------- */
    /*      Do additional work binding the geometry column.                 */
    /* -------------------------------------------------------------------- */
    if (iGeomColumn != -1)
    {
        OCIDefine *hGDefine = nullptr;

        if (poSession->Failed(
                OCIDefineByPos(poStatement->GetStatement(), &hGDefine,
                               poSession->hError, (ub4)iGeomColumn + 1,
                               static_cast<dvoid *>(nullptr), (sb4)0, SQLT_NTY,
                               static_cast<dvoid *>(nullptr),
                               static_cast<ub2 *>(nullptr),
                               static_cast<ub2 *>(nullptr), (ub4)OCI_DEFAULT),
                "OCIDefineByPos(geometry)"))
            return FALSE;

        if (poSession->Failed(
                OCIDefineObject(hGDefine, poSession->hError,
                                poSession->hGeometryTDO,
                                reinterpret_cast<dvoid **>(&hLastGeom),
                                static_cast<ub4 *>(nullptr),
                                reinterpret_cast<dvoid **>(&hLastGeomInd),
                                static_cast<ub4 *>(nullptr)),
                "OCIDefineObject"))
            return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                         TranslateGeometry()                          */
/************************************************************************/

OGRGeometry *OGROCILayer::TranslateGeometry()

{
    OGROCISession *poSession = poDS->GetSession();

    /* -------------------------------------------------------------------- */
    /*      Is the geometry NULL?                                           */
    /* -------------------------------------------------------------------- */
    if (hLastGeom == nullptr || hLastGeomInd == nullptr ||
        hLastGeomInd->_atomic == OCI_IND_NULL)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Get the size of the sdo_elem_info and sdo_ordinates arrays.     */
    /* -------------------------------------------------------------------- */
    int nElemCount, nOrdCount;

    if (poSession->Failed(
            OCICollSize(poSession->hEnv, poSession->hError,
                        reinterpret_cast<OCIColl *>(hLastGeom->sdo_elem_info),
                        &nElemCount),
            "OCICollSize(sdo_elem_info)"))
        return nullptr;

    if (poSession->Failed(
            OCICollSize(poSession->hEnv, poSession->hError,
                        reinterpret_cast<OCIColl *>(hLastGeom->sdo_ordinates),
                        &nOrdCount),
            "OCICollSize(sdo_ordinates)"))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Get the GType.                                                  */
    /* -------------------------------------------------------------------- */
    int nGType;

    if (poSession->Failed(OCINumberToInt(poSession->hError,
                                         &(hLastGeom->sdo_gtype),
                                         (uword)sizeof(int), OCI_NUMBER_SIGNED,
                                         static_cast<dvoid *>(&nGType)),
                          "OCINumberToInt(GType)"))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Establish the dimension.                                        */
    /* -------------------------------------------------------------------- */
    int nDimension = MAX(2, (nGType / 1000));

    /* -------------------------------------------------------------------- */
    /*      Handle point data directly from built-in point info.            */
    /* -------------------------------------------------------------------- */
    if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_POINT) &&
        hLastGeomInd->sdo_point._atomic == OCI_IND_NOTNULL &&
        hLastGeomInd->sdo_point.x == OCI_IND_NOTNULL &&
        hLastGeomInd->sdo_point.y == OCI_IND_NOTNULL)
    {
        double dfX, dfY, dfZ = 0.0;

        OCINumberToReal(poSession->hError, &(hLastGeom->sdo_point.x),
                        (uword)sizeof(double), static_cast<dvoid *>(&dfX));
        OCINumberToReal(poSession->hError, &(hLastGeom->sdo_point.y),
                        (uword)sizeof(double), static_cast<dvoid *>(&dfY));
        if (hLastGeomInd->sdo_point.z == OCI_IND_NOTNULL)
            OCINumberToReal(poSession->hError, &(hLastGeom->sdo_point.z),
                            (uword)sizeof(double), static_cast<dvoid *>(&dfZ));

        if (nDimension == 3)
            return new OGRPoint(dfX, dfY, dfZ);
        else
            return new OGRPoint(dfX, dfY);
    }

    /* -------------------------------------------------------------------- */
    /*      If this is a sort of container geometry, create the             */
    /*      container now.                                                  */
    /* -------------------------------------------------------------------- */
    OGRGeometryCollection *poCollection = nullptr;
    OGRPolygon *poPolygon = nullptr;

    if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_POLYGON))
        poPolygon = new OGRPolygon();
    else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_COLLECTION))
        poCollection = new OGRGeometryCollection();
    else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_MULTIPOINT))
        poCollection = new OGRMultiPoint();
    else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_MULTILINESTRING))
        poCollection = new OGRMultiLineString();
    else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_MULTIPOLYGON))
        poCollection = new OGRMultiPolygon();

    /* ==================================================================== */
    /*      Loop over the component elements.                               */
    /* ==================================================================== */
    for (int iElement = 0; iElement < nElemCount; iElement += 3)
    {
        int nInterpretation, nEType;
        int nStartOrdinal, nElemOrdCount;

        LoadElementInfo(iElement, nElemCount, nOrdCount, &nEType,
                        &nInterpretation, &nStartOrdinal, &nElemOrdCount);

        /* --------------------------------------------------------------------
         */
        /*      Translate this element. */
        /* --------------------------------------------------------------------
         */
        OGRGeometry *poGeom;

        poGeom = TranslateGeometryElement(&iElement, nGType, nDimension, nEType,
                                          nInterpretation, nStartOrdinal - 1,
                                          nElemOrdCount);

        if (poGeom == nullptr)
            return nullptr;

        /* --------------------------------------------------------------------
         */
        /*      Based on GType do what is appropriate. */
        /* --------------------------------------------------------------------
         */
        if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_LINESTRING))
        {
            CPLAssert(wkbFlatten(poGeom->getGeometryType()) == wkbLineString);
            return poGeom;
        }

        else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_POINT))
        {
            CPLAssert(wkbFlatten(poGeom->getGeometryType()) == wkbPoint);
            return poGeom;
        }

        else if (ORA_GTYPE_MATCH(nGType, ORA_GTYPE_POLYGON))
        {
            CPLAssert(wkbFlatten(poGeom->getGeometryType()) == wkbLineString);
            poPolygon->addRingDirectly(poGeom->toLinearRing());
        }
        else
        {
            CPLAssert(poCollection != nullptr);
            if (wkbFlatten(poGeom->getGeometryType()) == wkbMultiPoint)
            {
                OGRMultiPoint *poMP = poGeom->toMultiPoint();
                for (auto &&poPoint : *poMP)
                    poCollection->addGeometry(poPoint);
                delete poMP;
            }
            else if (nEType % 1000 == 3)
            {
                /* its one poly ring, create new poly or add to existing */
                if (nEType == 1003)
                {
                    if (poPolygon != nullptr &&
                        poPolygon->getExteriorRing() != nullptr)
                    {
                        poCollection->addGeometryDirectly(poPolygon);
                        poPolygon = nullptr;
                    }

                    poPolygon = new OGRPolygon();
                }

                if (poPolygon != nullptr)
                    poPolygon->addRingDirectly(poGeom->toLinearRing());
                else
                {
                    CPLAssert(poPolygon != nullptr);
                }
            }
            else
                poCollection->addGeometryDirectly(poGeom);
        }
    }

    if (poCollection != nullptr && poPolygon != nullptr)
        poCollection->addGeometryDirectly(poPolygon);

    /* -------------------------------------------------------------------- */
    /*      Return resulting collection geometry.                           */
    /* -------------------------------------------------------------------- */
    if (poCollection == nullptr)
        return poPolygon;
    else
        return poCollection;
}

/************************************************************************/
/*                          LoadElementInfo()                           */
/*                                                                      */
/*      Fetch the start ordinal, count, EType and interpretation        */
/*      values for a particular element.                                */
/************************************************************************/

int OGROCILayer::LoadElementInfo(int iElement, int nElemCount,
                                 int nTotalOrdCount, int *pnEType,
                                 int *pnInterpretation, int *pnStartOrdinal,
                                 int *pnElemOrdCount)

{
    OGROCISession *poSession = poDS->GetSession();
    boolean bExists;
    OCINumber *hNumber;
    /* -------------------------------------------------------------------- */
    /*      Get the details about element from the elem_info array.         */
    /* -------------------------------------------------------------------- */
    OCICollGetElem(poSession->hEnv, poSession->hError,
                   reinterpret_cast<OCIColl *>(hLastGeom->sdo_elem_info),
                   (sb4)(iElement + 0), reinterpret_cast<boolean *>(&bExists),
                   reinterpret_cast<dvoid **>(&hNumber), nullptr);
    OCINumberToInt(poSession->hError, hNumber, (uword)sizeof(ub4),
                   OCI_NUMBER_UNSIGNED, static_cast<dvoid *>(pnStartOrdinal));

    OCICollGetElem(poSession->hEnv, poSession->hError,
                   reinterpret_cast<OCIColl *>(hLastGeom->sdo_elem_info),
                   (sb4)(iElement + 1), reinterpret_cast<boolean *>(&bExists),
                   reinterpret_cast<dvoid **>(&hNumber), nullptr);
    OCINumberToInt(poSession->hError, hNumber, (uword)sizeof(ub4),
                   OCI_NUMBER_UNSIGNED, static_cast<dvoid *>(pnEType));

    OCICollGetElem(poSession->hEnv, poSession->hError,
                   reinterpret_cast<OCIColl *>(hLastGeom->sdo_elem_info),
                   (sb4)(iElement + 2), reinterpret_cast<boolean *>(&bExists),
                   reinterpret_cast<dvoid **>(&hNumber), nullptr);
    OCINumberToInt(poSession->hError, hNumber, (uword)sizeof(ub4),
                   OCI_NUMBER_UNSIGNED, static_cast<dvoid *>(pnInterpretation));

    if (iElement < nElemCount - 3)
    {
        ub4 nNextStartOrdinal;

        OCICollGetElem(poSession->hEnv, poSession->hError,
                       reinterpret_cast<OCIColl *>(hLastGeom->sdo_elem_info),
                       (sb4)(iElement + 3),
                       reinterpret_cast<boolean *>(&bExists),
                       reinterpret_cast<dvoid **>(&hNumber), nullptr);
        OCINumberToInt(poSession->hError, hNumber, (uword)sizeof(ub4),
                       OCI_NUMBER_UNSIGNED,
                       static_cast<dvoid *>(&nNextStartOrdinal));

        *pnElemOrdCount = nNextStartOrdinal - *pnStartOrdinal;
    }
    else
        *pnElemOrdCount = nTotalOrdCount - *pnStartOrdinal + 1;

    return TRUE;
}

/************************************************************************/
/*                      TranslateGeometryElement()                      */
/************************************************************************/

OGRGeometry *OGROCILayer::TranslateGeometryElement(int *piElement, int nGType,
                                                   int nDimension, int nEType,
                                                   int nInterpretation,
                                                   int nStartOrdinal,
                                                   int nElemOrdCount)

{
    /* -------------------------------------------------------------------- */
    /*      Handle simple point.                                            */
    /* -------------------------------------------------------------------- */
    if (nEType == 1 && nInterpretation == 1)
    {
        OGRPoint *poPoint = new OGRPoint();
        double dfX, dfY, dfZ = 0.0;

        GetOrdinalPoint(nStartOrdinal, nDimension, &dfX, &dfY, &dfZ);

        poPoint->setX(dfX);
        poPoint->setY(dfY);
        if (nDimension == 3)
            poPoint->setZ(dfZ);

        return poPoint;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle multipoint.                                              */
    /* -------------------------------------------------------------------- */
    else if (nEType == 1 && nInterpretation > 1)
    {
        OGRMultiPoint *poMP = new OGRMultiPoint();
        double dfX, dfY, dfZ = 0.0;
        int i;

        CPLAssert(nInterpretation == nElemOrdCount / nDimension);

        for (i = 0; i < nInterpretation; i++)
        {
            GetOrdinalPoint(nStartOrdinal + i * nDimension, nDimension, &dfX,
                            &dfY, &dfZ);

            OGRPoint *poPoint = (nDimension == 3) ? new OGRPoint(dfX, dfY, dfZ)
                                                  : new OGRPoint(dfX, dfY);
            poMP->addGeometryDirectly(poPoint);
        }
        return poMP;
    }

    /* -------------------------------------------------------------------- */
    /*      Discard orientations for oriented points.                       */
    /* -------------------------------------------------------------------- */
    else if (nEType == 1 && nInterpretation == 0)
    {
        CPLDebug("OCI", "Ignoring orientations for oriented points.");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle line strings consisting of straight segments.            */
    /* -------------------------------------------------------------------- */
    else if (nEType == 2 && nInterpretation == 1)
    {
        OGRLineString *poLS = new OGRLineString();
        int nPointCount = nElemOrdCount / nDimension, i;

        poLS->setNumPoints(nPointCount);

        for (i = 0; i < nPointCount; i++)
        {
            double dfX, dfY, dfZ = 0.0;

            GetOrdinalPoint(i * nDimension + nStartOrdinal, nDimension, &dfX,
                            &dfY, &dfZ);
            if (nDimension == 3)
                poLS->setPoint(i, dfX, dfY, dfZ);
            else
                poLS->setPoint(i, dfX, dfY);
        }

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle line strings consisting of circular arcs.                */
    /* -------------------------------------------------------------------- */
    else if (nEType == 2 && nInterpretation == 2)
    {
        OGRLineString *poLS = new OGRLineString();
        int nPointCount = nElemOrdCount / nDimension, i;

        for (i = 0; i < nPointCount - 2; i += 2)
        {
            double dfStartX, dfStartY, dfStartZ = 0.0;
            double dfMidX, dfMidY, dfMidZ = 0.0;
            double dfEndX, dfEndY, dfEndZ = 0.0;

            GetOrdinalPoint(i * nDimension + nStartOrdinal, nDimension,
                            &dfStartX, &dfStartY, &dfStartZ);
            GetOrdinalPoint((i + 1) * nDimension + nStartOrdinal, nDimension,
                            &dfMidX, &dfMidY, &dfMidZ);
            GetOrdinalPoint((i + 2) * nDimension + nStartOrdinal, nDimension,
                            &dfEndX, &dfEndY, &dfEndZ);

            OGROCIStrokeArcToOGRGeometry_Points(dfStartX, dfStartY, dfMidX,
                                                dfMidY, dfEndX, dfEndY, 6.0,
                                                FALSE, poLS);
        }

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle polygon rings.  Treat curves as if they were             */
    /*      linestrings.                                                    */
    /* -------------------------------------------------------------------- */
    else if (nEType % 1000 == 3 && nInterpretation == 1)
    {
        OGRLinearRing *poLS = new OGRLinearRing();
        int nPointCount = nElemOrdCount / nDimension, i;

        poLS->setNumPoints(nPointCount);

        for (i = 0; i < nPointCount; i++)
        {
            double dfX, dfY, dfZ = 0.0;

            GetOrdinalPoint(i * nDimension + nStartOrdinal, nDimension, &dfX,
                            &dfY, &dfZ);
            if (nDimension == 3)
                poLS->setPoint(i, dfX, dfY, dfZ);
            else
                poLS->setPoint(i, dfX, dfY);
        }

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle polygon rings made of circular arcs.                     */
    /* -------------------------------------------------------------------- */
    else if (nEType % 1000 == 3 && nInterpretation == 2)
    {
        OGRLineString *poLS = new OGRLinearRing();
        int nPointCount = nElemOrdCount / nDimension, i;

        for (i = 0; i < nPointCount - 2; i += 2)
        {
            double dfStartX, dfStartY, dfStartZ = 0.0;
            double dfMidX, dfMidY, dfMidZ = 0.0;
            double dfEndX, dfEndY, dfEndZ = 0.0;

            GetOrdinalPoint(i * nDimension + nStartOrdinal, nDimension,
                            &dfStartX, &dfStartY, &dfStartZ);
            GetOrdinalPoint((i + 1) * nDimension + nStartOrdinal, nDimension,
                            &dfMidX, &dfMidY, &dfMidZ);
            GetOrdinalPoint((i + 2) * nDimension + nStartOrdinal, nDimension,
                            &dfEndX, &dfEndY, &dfEndZ);

            OGROCIStrokeArcToOGRGeometry_Points(dfStartX, dfStartY, dfMidX,
                                                dfMidY, dfEndX, dfEndY, 6.0,
                                                FALSE, poLS);
        }

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle rectangle definitions ... translate into a linear ring.  */
    /* -------------------------------------------------------------------- */
    else if (nEType % 1000 == 3 && nInterpretation == 3)
    {
        OGRLinearRing *poLS = new OGRLinearRing();
        double dfX1, dfY1, dfZ1 = 0.0;
        double dfX2, dfY2, dfZ2 = 0.0;

        GetOrdinalPoint(nStartOrdinal, nDimension, &dfX1, &dfY1, &dfZ1);
        GetOrdinalPoint(nStartOrdinal + nDimension, nDimension, &dfX2, &dfY2,
                        &dfZ2);

        poLS->setNumPoints(5);

        poLS->setPoint(0, dfX1, dfY1, dfZ1);
        poLS->setPoint(1, dfX2, dfY1, dfZ1);
        poLS->setPoint(2, dfX2, dfY2, dfZ2);
        poLS->setPoint(3, dfX1, dfY2, dfZ2);
        poLS->setPoint(4, dfX1, dfY1, dfZ1);

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle circle definitions ... translate into a linear ring.     */
    /* -------------------------------------------------------------------- */
    else if (nEType % 100 == 3 && nInterpretation == 4)
    {
        OGRLinearRing *poLS = new OGRLinearRing();
        double dfX1, dfY1, dfZ1 = 0.0;
        double dfX2, dfY2, dfZ2 = 0.0;
        double dfX3, dfY3, dfZ3 = 0.0;

        GetOrdinalPoint(nStartOrdinal, nDimension, &dfX1, &dfY1, &dfZ1);
        GetOrdinalPoint(nStartOrdinal + nDimension, nDimension, &dfX2, &dfY2,
                        &dfZ2);
        GetOrdinalPoint(nStartOrdinal + nDimension * 2, nDimension, &dfX3,
                        &dfY3, &dfZ3);

        OGROCIStrokeArcToOGRGeometry_Points(dfX1, dfY1, dfX2, dfY2, dfX3, dfY3,
                                            6.0, TRUE, poLS);

        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle compound line strings and polygon rings.                 */
    /*                                                                      */
    /*      This is quite complicated since we need to consume several      */
    /*      following elements, and merge the resulting geometries.         */
    /* -------------------------------------------------------------------- */
    else if (nEType == 4 || nEType % 100 == 5)
    {
        int nSubElementCount = nInterpretation;
        OGRLineString *poLS;
        int nElemCount, nTotalOrdCount;
        OGROCISession *poSession = poDS->GetSession();

        if (poSession->Failed(OCICollSize(poSession->hEnv, poSession->hError,
                                          reinterpret_cast<OCIColl *>(
                                              hLastGeom->sdo_elem_info),
                                          &nElemCount),
                              "OCICollSize(sdo_elem_info)"))
            return nullptr;

        if (poSession->Failed(OCICollSize(poSession->hEnv, poSession->hError,
                                          reinterpret_cast<OCIColl *>(
                                              hLastGeom->sdo_ordinates),
                                          &nTotalOrdCount),
                              "OCICollSize(sdo_ordinates)"))
            return nullptr;

        if (nEType == 4)
            poLS = new OGRLineString();
        else
            poLS = new OGRLinearRing();

        for (*piElement += 3; nSubElementCount-- > 0; *piElement += 3)
        {
            LoadElementInfo(*piElement, nElemCount, nTotalOrdCount, &nEType,
                            &nInterpretation, &nStartOrdinal, &nElemOrdCount);

            // Adjust for repeated end point except for last element.
            if (nSubElementCount > 0)
                nElemOrdCount += nDimension;

            // translate element.
            OGRGeometry *poGeom = TranslateGeometryElement(
                piElement, nGType, nDimension, nEType, nInterpretation,
                nStartOrdinal - 1, nElemOrdCount);
            OGRLineString *poElemLS = dynamic_cast<OGRLineString *>(poGeom);

            // Try to append to our aggregate linestring/ring
            if (poElemLS)
            {
                if (poLS->getNumPoints() > 0)
                {
                    CPLAssert(poElemLS->getX(0) ==
                                  poLS->getX(poLS->getNumPoints() - 1) &&
                              poElemLS->getY(0) ==
                                  poLS->getY(poLS->getNumPoints() - 1));

                    poLS->addSubLineString(poElemLS, 1);
                }
                else
                    poLS->addSubLineString(poElemLS, 0);
            }
            delete poGeom;
        }

        *piElement -= 3;
        return poLS;
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise it is apparently unsupported.                         */
    /* -------------------------------------------------------------------- */
    else
    {

        CPLDebug("OCI", "Geometry with EType=%d, Interp=%d ignored.", nEType,
                 nInterpretation);
    }

    return nullptr;
}

/************************************************************************/
/*                          GetOrdinalPoint()                           */
/************************************************************************/

int OGROCILayer::GetOrdinalPoint(int iOrdinal, int nDimension, double *pdfX,
                                 double *pdfY, double *pdfZ)

{
    OGROCISession *poSession = poDS->GetSession();
    boolean bExists;
    OCINumber *hNumber;

    OCICollGetElem(poSession->hEnv, poSession->hError,
                   reinterpret_cast<OCIColl *>(hLastGeom->sdo_ordinates),
                   (sb4)iOrdinal + 0, reinterpret_cast<boolean *>(&bExists),
                   reinterpret_cast<dvoid **>(&hNumber), nullptr);
    OCINumberToReal(poSession->hError, hNumber, (uword)sizeof(double),
                    static_cast<dvoid *>(pdfX));
    OCICollGetElem(poSession->hEnv, poSession->hError,
                   reinterpret_cast<OCIColl *>(hLastGeom->sdo_ordinates),
                   (sb4)iOrdinal + 1, reinterpret_cast<boolean *>(&bExists),
                   reinterpret_cast<dvoid **>(&hNumber), nullptr);
    OCINumberToReal(poSession->hError, hNumber, (uword)sizeof(double),
                    static_cast<dvoid *>(pdfY));
    if (nDimension == 3)
    {
        OCICollGetElem(poSession->hEnv, poSession->hError,
                       reinterpret_cast<OCIColl *>(hLastGeom->sdo_ordinates),
                       (sb4)iOrdinal + 2, reinterpret_cast<boolean *>(&bExists),
                       reinterpret_cast<dvoid **>(&hNumber), nullptr);
        OCINumberToReal(poSession->hError, hNumber, (uword)sizeof(double),
                        static_cast<dvoid *>(pdfZ));
    }

    return TRUE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGROCILayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCRandomRead))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastFeatureCount))
        return m_poFilterGeom == nullptr;

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
        return TRUE;

    else if (EQUAL(pszCap, OLCTransactions))
        return TRUE;

    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    else
        return FALSE;
}

/************************************************************************/
/*                          LookupTableSRID()                           */
/*                                                                      */
/*      Note that the table name may also be prefixed by the owner      */
/*      with a dot separator.                                           */
/************************************************************************/

int OGROCILayer::LookupTableSRID()

{
    /* -------------------------------------------------------------------- */
    /*      If we don't have a geometry column, there isn't much point      */
    /*      in trying.                                                      */
    /* -------------------------------------------------------------------- */
    if (pszGeomName == nullptr)
        return -1;

    /* -------------------------------------------------------------------- */
    /*      Split out the owner if available.                               */
    /* -------------------------------------------------------------------- */
    const char *pszTableName = GetLayerDefn()->GetName();
    char *pszOwner = nullptr;

    if (strstr(pszTableName, ".") != nullptr)
    {
        pszOwner = CPLStrdup(pszTableName);
        pszTableName = strstr(pszTableName, ".") + 1;

        *(strstr(pszOwner, ".")) = '\0';
    }

    /* -------------------------------------------------------------------- */
    /*      Build our query command.                                        */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf oCommand;

    oCommand.Append("SELECT SRID FROM ALL_SDO_GEOM_METADATA "
                    "WHERE TABLE_NAME = UPPER(:table_name) AND COLUMN_NAME = "
                    "UPPER(:geometry_name)");

    if (pszOwner != nullptr)
    {
        oCommand.Append(" AND OWNER = :owner");
    }

    /* -------------------------------------------------------------------- */
    /*      Execute query command.                                          */
    /* -------------------------------------------------------------------- */
    OGROCIStatement oGetTables(poDS->GetSession());
    int nSRID = -1;

    if (oGetTables.Prepare(oCommand.GetString()) != CE_None)
        return nSRID;

    oGetTables.BindString(":table_name", pszTableName);
    oGetTables.BindString(":geometry_name", pszGeomName);
    if (pszOwner != nullptr)
    {
        oGetTables.BindString(":owner", pszOwner);
        CPLFree(pszOwner);
    }

    if (oGetTables.Execute(nullptr) == CE_None)
    {
        char **papszRow = oGetTables.SimpleFetchRow();

        if (papszRow != nullptr && papszRow[0] != nullptr)
            nSRID = atoi(papszRow[0]);
    }

    return nSRID;
}

/************************************************************************/
/*                            GetFIDColumn()                            */
/************************************************************************/

const char *OGROCILayer::GetFIDColumn()

{
    if (pszFIDName != nullptr)
        return pszFIDName;
    else
        return "";
}

/************************************************************************/
/*                         GetGeometryColumn()                          */
/************************************************************************/

const char *OGROCILayer::GetGeometryColumn()

{
    if (pszGeomName != nullptr)
        return pszGeomName;
    else
        return "";
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGROCILayer::GetDataset()
{
    return poDS;
}
