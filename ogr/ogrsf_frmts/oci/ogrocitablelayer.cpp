/******************************************************************************
 *
 * Project:  Oracle Spatial Driver
 * Purpose:  Implementation of the OGROCITableLayer class.  This class provides
 *           layer semantics on a table, but utilizing a lot of machinery from
 *           the OGROCILayer base class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_oci.h"
#include "cpl_conv.h"
#include "cpl_string.h"

#include <cmath>

static int nDiscarded = 0;
static int nHits = 0;

#define HSI_UNKNOWN -2

/************************************************************************/
/*                          OGROCITableLayer()                          */
/************************************************************************/

OGROCITableLayer::OGROCITableLayer(OGROCIDataSource *poDSIn,
                                   const char *pszTableName,
                                   OGRwkbGeometryType eGType, int nSRIDIn,
                                   int bUpdate, int bNewLayerIn)
    : OGROCIWritableLayer(poDSIn)

{
    bExtentUpdated = false;

    pszQuery = nullptr;
    pszWHERE = CPLStrdup("");
    pszQueryStatement = nullptr;

    bUpdateAccess = bUpdate;
    bNewLayer = bNewLayerIn;

    iNextShapeId = 0;
    iNextFIDToWrite = -1;

    bValidTable = FALSE;
    if (bNewLayerIn)
        bHaveSpatialIndex = FALSE;
    else
        bHaveSpatialIndex = HSI_UNKNOWN;

    poFeatureDefn = ReadTableDefinition(pszTableName);
    if (eGType != wkbUnknown && poFeatureDefn->GetGeomFieldCount() > 0)
        poFeatureDefn->GetGeomFieldDefn(0)->SetType(eGType);
    SetDescription(poFeatureDefn->GetName());

    nSRID = nSRIDIn;
    if (nSRID == -1)
        nSRID = LookupTableSRID();

    poSRS = poDSIn->FetchSRS(nSRID);
    if (poSRS != nullptr)
        poSRS->Reference();

    hOrdVARRAY = nullptr;
    hElemInfoVARRAY = nullptr;

    poBoundStatement = nullptr;

    nWriteCacheMax = 0;
    nWriteCacheUsed = 0;
    pasWriteGeoms = nullptr;
    papsWriteGeomMap = nullptr;
    pasWriteGeomInd = nullptr;
    papsWriteGeomIndMap = nullptr;

    papWriteFields = nullptr;
    papaeWriteFieldInd = nullptr;

    panWriteFIDs = nullptr;

    nDefaultStringSize = 4000;

    OGROCITableLayer::ResetReading();
}

/************************************************************************/
/*                         ~OGROCITableLayer()                          */
/************************************************************************/

OGROCITableLayer::~OGROCITableLayer()

{
    int i;

    OGROCITableLayer::SyncToDisk();

    CPLFree(panWriteFIDs);
    if (papWriteFields != nullptr)
    {
        for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
        {
            CPLFree(papWriteFields[i]);
            CPLFree(papaeWriteFieldInd[i]);
        }
    }

    CPLFree(papWriteFields);
    CPLFree(papaeWriteFieldInd);

    if (poBoundStatement != nullptr)
        delete poBoundStatement;

    CPLFree(pasWriteGeomInd);
    CPLFree(papsWriteGeomIndMap);

    CPLFree(papsWriteGeomMap);
    CPLFree(pasWriteGeoms);

    CPLFree(pszQuery);
    CPLFree(pszWHERE);

    if (poSRS != nullptr && poSRS->Dereference() == 0)
        delete poSRS;
}

/************************************************************************/
/*                        ReadTableDefinition()                         */
/*                                                                      */
/*      Build a schema from the named table.  Done by querying the      */
/*      catalog.                                                        */
/************************************************************************/

OGRFeatureDefn *OGROCITableLayer::ReadTableDefinition(const char *pszTable)

{
    OGROCISession *poSession = poDS->GetSession();
    sword nStatus;

    CPLString osUnquotedTableName;
    CPLString osQuotedTableName;

    /* -------------------------------------------------------------------- */
    /*      Split out the owner if available.                               */
    /* -------------------------------------------------------------------- */
    if (strstr(pszTable, ".") != nullptr)
    {
        osTableName = strstr(pszTable, ".") + 1;
        osOwner.assign(pszTable, strlen(pszTable) - osTableName.size() - 1);
        osUnquotedTableName.Printf("%s.%s", osOwner.c_str(),
                                   osTableName.c_str());
        osQuotedTableName.Printf("\"%s\".\"%s\"", osOwner.c_str(),
                                 osTableName.c_str());
    }
    else
    {
        osTableName = pszTable;
        osOwner = "";
        osUnquotedTableName.Printf("%s", pszTable);
        osQuotedTableName.Printf("\"%s\"", pszTable);
    }

    OGRFeatureDefn *poDefn = new OGRFeatureDefn(osUnquotedTableName.c_str());

    poDefn->Reference();

    /* -------------------------------------------------------------------- */
    /*      Do a DescribeAll on the table.                                  */
    /* -------------------------------------------------------------------- */
    OCIParam *hAttrParam = nullptr;
    OCIParam *hAttrList = nullptr;

    // Table name unquoted

    nStatus = OCIDescribeAny(poSession->hSvcCtx, poSession->hError,
                             (dvoid *)osUnquotedTableName.c_str(),
                             static_cast<ub4>(osUnquotedTableName.length()),
                             OCI_OTYPE_NAME, OCI_DEFAULT, OCI_PTYPE_TABLE,
                             poSession->hDescribe);

    if (poSession->Failed(nStatus, "OCIDescribeAny"))
    {
        CPLErrorReset();

        // View name unquoted

        nStatus = OCIDescribeAny(poSession->hSvcCtx, poSession->hError,
                                 (dvoid *)osUnquotedTableName.c_str(),
                                 static_cast<ub4>(osUnquotedTableName.length()),
                                 OCI_OTYPE_NAME, OCI_DEFAULT, OCI_PTYPE_VIEW,
                                 poSession->hDescribe);

        if (poSession->Failed(nStatus, "OCIDescribeAny"))
        {
            CPLErrorReset();

            // Table name quoted

            nStatus = OCIDescribeAny(
                poSession->hSvcCtx, poSession->hError,
                (dvoid *)osQuotedTableName.c_str(),
                static_cast<ub4>(osQuotedTableName.length()), OCI_OTYPE_NAME,
                OCI_DEFAULT, OCI_PTYPE_TABLE, poSession->hDescribe);

            if (poSession->Failed(nStatus, "OCIDescribeAny"))
            {
                CPLErrorReset();

                // View name quoted

                nStatus =
                    OCIDescribeAny(poSession->hSvcCtx, poSession->hError,
                                   (dvoid *)osQuotedTableName.c_str(),
                                   static_cast<ub4>(osQuotedTableName.length()),
                                   OCI_OTYPE_NAME, OCI_DEFAULT, OCI_PTYPE_VIEW,
                                   poSession->hDescribe);

                if (poSession->Failed(nStatus, "OCIDescribeAny"))
                    return poDefn;
            }
        }
    }

    if (poSession->Failed(OCIAttrGet(poSession->hDescribe, OCI_HTYPE_DESCRIBE,
                                     &hAttrParam, nullptr, OCI_ATTR_PARAM,
                                     poSession->hError),
                          "OCIAttrGet(ATTR_PARAM)"))
        return poDefn;

    if (poSession->Failed(OCIAttrGet(hAttrParam, OCI_DTYPE_PARAM, &hAttrList,
                                     nullptr, OCI_ATTR_LIST_COLUMNS,
                                     poSession->hError),
                          "OCIAttrGet(ATTR_LIST_COLUMNS)"))
        return poDefn;

    /* -------------------------------------------------------------------- */
    /*      What is the name of the column to use as FID?  This defaults    */
    /*      to OGR_FID but we allow it to be overridden by a config         */
    /*      variable.  Ideally we would identify a column that is a         */
    /*      primary key and use that, but I'm not yet sure how to           */
    /*      accomplish that.                                                */
    /* -------------------------------------------------------------------- */
    const char *pszExpectedFIDName = CPLGetConfigOption("OCI_FID", "OGR_FID");
    int bGeomFieldNullable = FALSE;

    /* -------------------------------------------------------------------- */
    /*      Parse the returned table information.                           */
    /* -------------------------------------------------------------------- */
    for (int iRawFld = 0; true; iRawFld++)
    {
        OGRFieldDefn oField("", OFTString);
        OCIParam *hParamDesc;
        ub2 nOCIType;
        ub4 nOCILen;

        nStatus = OCIParamGet(hAttrList, OCI_DTYPE_PARAM, poSession->hError,
                              reinterpret_cast<dvoid **>(&hParamDesc),
                              (ub4)iRawFld + 1);
        if (nStatus != OCI_SUCCESS)
            break;

        if (poSession->GetParamInfo(hParamDesc, &oField, &nOCIType, &nOCILen) !=
            CE_None)
            return poDefn;

        if (oField.GetType() == OFTBinary)
        {
            if (nOCIType == 108 && pszGeomName == nullptr)
            {
                CPLFree(pszGeomName);
                pszGeomName = CPLStrdup(oField.GetNameRef());
                iGeomColumn = iRawFld;
                bGeomFieldNullable = oField.IsNullable();
            }
            continue;
        }

        if (EQUAL(oField.GetNameRef(), pszExpectedFIDName) &&
            (oField.GetType() == OFTInteger ||
             oField.GetType() == OFTInteger64))
        {
            pszFIDName = CPLStrdup(oField.GetNameRef());
            continue;
        }

        if (oField.GetTZFlag() >= OGR_TZFLAG_MIXED_TZ)
        {
            setFieldIndexWithTimeStampWithTZ.insert(poDefn->GetFieldCount());
        }

        poDefn->AddFieldDefn(&oField);
    }

    OGROCIStatement defaultValuesStatement(poSession);

    const char *pszDefaultValueSQL =
        "SELECT COLUMN_NAME, DATA_DEFAULT\n"
        "FROM user_tab_columns\n"
        "WHERE DATA_DEFAULT IS NOT NULL AND TABLE_NAME = UPPER(:table_name)";

    defaultValuesStatement.Prepare(pszDefaultValueSQL);
    defaultValuesStatement.BindString(":table_name", pszTable);

    if (defaultValuesStatement.Execute(nullptr) == CE_None)
    {
        char **papszRow;

        while ((papszRow = defaultValuesStatement.SimpleFetchRow()) != nullptr)
        {
            const char *pszColName = papszRow[0];
            const char *pszDefault = papszRow[1];
            int nIdx = poDefn->GetFieldIndex(pszColName);
            if (nIdx >= 0)
                poDefn->GetFieldDefn(nIdx)->SetDefault(pszDefault);
        }
    }

    if (EQUAL(pszExpectedFIDName, "OGR_FID") && pszFIDName)
    {
        for (int i = 0; i < poDefn->GetFieldCount(); i++)
        {
            // This is presumably a Integer since we always create Integer64
            // with a defined precision
            if (poDefn->GetFieldDefn(i)->GetType() == OFTInteger64 &&
                poDefn->GetFieldDefn(i)->GetWidth() == 0)
            {
                poDefn->GetFieldDefn(i)->SetType(OFTInteger);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Identify Geometry dimension                                     */
    /* -------------------------------------------------------------------- */

    if (pszGeomName != nullptr && strlen(pszGeomName) > 0)
    {
        OGROCIStatement oDimStatement(poSession);
        char **papszResult;
        int iDim = -1;

        if (osOwner != "")
        {
            const char *pszDimCmdA =
                "SELECT COUNT(*)\n"
                "FROM ALL_SDO_GEOM_METADATA u, TABLE(u.diminfo) t\n"
                "WHERE u.table_name = :table_name\n"
                "  AND u.column_name = :geometry_name\n"
                "  AND u.owner = :table_owner";

            oDimStatement.Prepare(pszDimCmdA);
            oDimStatement.BindString(":table_name", osTableName.c_str());
            oDimStatement.BindString(":geometry_name", pszGeomName);
            oDimStatement.BindString(":table_owner", osOwner.c_str());
        }
        else
        {
            const char *pszDimCmdB =
                "SELECT COUNT(*)\n"
                "FROM USER_SDO_GEOM_METADATA u, TABLE(u.diminfo) t\n"
                "WHERE u.table_name = :table_name\n"
                "  AND u.column_name = :geometry_name";

            oDimStatement.Prepare(pszDimCmdB);
            oDimStatement.BindString(":table_name", osTableName.c_str());
            oDimStatement.BindString(":geometry_name", pszGeomName);
        }
        oDimStatement.Execute(nullptr);

        papszResult = oDimStatement.SimpleFetchRow();

        if (CSLCount(papszResult) < 1)
        {
            OGROCIStatement oDimStatement2(poSession);
            char **papszResult2;

            CPLErrorReset();

            if (osOwner != "")
            {
                const char *pszDimCmd2A =
                    "select m.sdo_index_dims\n"
                    "from   all_sdo_index_metadata m, all_sdo_index_info i\n"
                    "where  i.index_name = m.sdo_index_name\n"
                    "   and i.sdo_index_owner = m.sdo_index_owner\n"
                    "   and i.sdo_index_owner = upper(:table_owner)\n"
                    "   and i.table_name = upper(:table_name)";

                oDimStatement2.Prepare(pszDimCmd2A);
                oDimStatement2.BindString(":table_owner", osOwner.c_str());
                oDimStatement2.BindString(":table_name", osTableName.c_str());
            }
            else
            {
                const char *pszDimCmd2B =
                    "select m.sdo_index_dims\n"
                    "from   user_sdo_index_metadata m, user_sdo_index_info i\n"
                    "where  i.index_name = m.sdo_index_name\n"
                    "   and i.table_name = upper(:table_name)";

                oDimStatement2.Prepare(pszDimCmd2B);
                oDimStatement2.BindString(":table_name", osTableName.c_str());
            }
            oDimStatement2.Execute(nullptr);

            papszResult2 = oDimStatement2.SimpleFetchRow();

            if (CSLCount(papszResult2) > 0)
            {
                iDim = atoi(papszResult2[0]);
            }
            else
            {
                // we want to clear any errors to avoid confusing the
                // application.
                CPLErrorReset();
            }
        }
        else
        {
            iDim = atoi(papszResult[0]);
        }

        if (iDim > 0)
        {
            SetDimension(iDim);
        }
        else
        {
            CPLDebug("OCI", "get dim based of existing data or index failed.");
        }

        {
            OGROCIStatement oDimStatement2(poSession);
            char **papszResult2;

            CPLErrorReset();
            if (osOwner != "")
            {
                const char *pszLayerTypeCmdA =
                    "select m.SDO_LAYER_GTYPE "
                    "from all_sdo_index_metadata m, all_sdo_index_info i "
                    "where i.index_name = m.sdo_index_name "
                    "and i.sdo_index_owner = m.sdo_index_owner "
                    "and i.sdo_index_owner = upper(:table_owner) "
                    "and i.table_name = upper(:table_name)";

                oDimStatement2.Prepare(pszLayerTypeCmdA);
                oDimStatement2.BindString(":table_owner", osOwner.c_str());
                oDimStatement2.BindString(":table_name", osTableName.c_str());
            }
            else
            {
                const char *pszLayerTypeCmdB =
                    "select m.SDO_LAYER_GTYPE "
                    "from user_sdo_index_metadata m, user_sdo_index_info i "
                    "where i.index_name = m.sdo_index_name "
                    "and i.table_name = upper(:table_name)";
                oDimStatement2.Prepare(pszLayerTypeCmdB);
                oDimStatement2.BindString(":table_name", osTableName.c_str());
            }

            oDimStatement2.Execute(nullptr);

            papszResult2 = oDimStatement2.SimpleFetchRow();

            if (CSLCount(papszResult2) > 0)
            {
                const char *pszLayerGType = papszResult2[0];
                OGRwkbGeometryType eGeomType = wkbUnknown;
                if (EQUAL(pszLayerGType, "POINT"))
                    eGeomType = wkbPoint;
                else if (EQUAL(pszLayerGType, "LINE"))
                    eGeomType = wkbLineString;
                else if (EQUAL(pszLayerGType, "POLYGON"))
                    eGeomType = wkbPolygon;
                else if (EQUAL(pszLayerGType, "MULTIPOINT"))
                    eGeomType = wkbMultiPoint;
                else if (EQUAL(pszLayerGType, "MULTILINE"))
                    eGeomType = wkbMultiLineString;
                else if (EQUAL(pszLayerGType, "MULTIPOLYGON"))
                    eGeomType = wkbMultiPolygon;
                else if (!EQUAL(pszLayerGType, "COLLECTION"))
                    CPLDebug("OCI", "LAYER_GTYPE = %s", pszLayerGType);
                if (iDim == 3)
                    eGeomType = wkbSetZ(eGeomType);
                poDefn->GetGeomFieldDefn(0)->SetType(eGeomType);
                poDefn->GetGeomFieldDefn(0)->SetNullable(bGeomFieldNullable);
            }
            else
            {
                // we want to clear any errors to avoid confusing the
                // application.
                CPLErrorReset();
            }
        }
    }
    else
    {
        poDefn->SetGeomType(wkbNone);
    }

    bValidTable = TRUE;

    return poDefn;
}

/************************************************************************/
/*                          ISetSpatialFilter()                         */
/************************************************************************/

OGRErr OGROCITableLayer::ISetSpatialFilter(int, const OGRGeometry *poGeomIn)

{
    if (!InstallFilter(poGeomIn))
        return OGRERR_NONE;

    BuildWhere();

    ResetReading();
    return OGRERR_NONE;
}

/************************************************************************/
/*                        TestForSpatialIndex()                         */
/************************************************************************/

void OGROCITableLayer::TestForSpatialIndex(const char *pszSpatWHERE)

{
    OGROCIStringBuf oTestCmd;
    OGROCIStatement oTestStatement(poDS->GetSession());

    oTestCmd.Append("SELECT COUNT(*) FROM ");
    oTestCmd.Append(poFeatureDefn->GetName());
    oTestCmd.Append(pszSpatWHERE);

    if (oTestStatement.Execute(oTestCmd.GetString()) != CE_None)
        bHaveSpatialIndex = FALSE;
    else
        bHaveSpatialIndex = TRUE;
}

/************************************************************************/
/*                             BuildWhere()                             */
/*                                                                      */
/*      Build the WHERE statement appropriate to the current set of     */
/*      criteria (spatial and attribute queries).                       */
/************************************************************************/

void OGROCITableLayer::BuildWhere()

{
    OGROCIStringBuf oWHERE;

    CPLFree(pszWHERE);
    pszWHERE = nullptr;

    if (m_poFilterGeom != nullptr && bHaveSpatialIndex)
    {
        OGREnvelope sEnvelope;

        m_poFilterGeom->getEnvelope(&sEnvelope);

        oWHERE.Append(" WHERE sdo_filter(");
        oWHERE.Append(pszGeomName);
        oWHERE.Append(", MDSYS.SDO_GEOMETRY(2003,");
        if (nSRID == -1)
            oWHERE.Append("NULL");
        else
            oWHERE.Appendf(15, "%d", nSRID);
        oWHERE.Append(",NULL,");
        oWHERE.Append("MDSYS.SDO_ELEM_INFO_ARRAY(1,1003,1),");
        oWHERE.Append("MDSYS.SDO_ORDINATE_ARRAY(");
        oWHERE.Appendf(
            600, "%.16g,%.16g,%.16g,%.16g,%.16g,%.16g,%.16g,%.16g,%.16g,%.16g",
            sEnvelope.MinX, sEnvelope.MinY, sEnvelope.MaxX, sEnvelope.MinY,
            sEnvelope.MaxX, sEnvelope.MaxY, sEnvelope.MinX, sEnvelope.MaxY,
            sEnvelope.MinX, sEnvelope.MinY);
        oWHERE.Append(")), 'querytype=window') = 'TRUE' ");
    }

    if (bHaveSpatialIndex == HSI_UNKNOWN)
    {
        TestForSpatialIndex(oWHERE.GetString());
        if (!bHaveSpatialIndex)
            oWHERE.Clear();
    }

    if (pszQuery != nullptr)
    {
        if (oWHERE.GetLast() == '\0')
            oWHERE.Append("WHERE ");
        else
            oWHERE.Append("AND ");

        oWHERE.Append(pszQuery);
    }

    pszWHERE = oWHERE.StealString();
}

/************************************************************************/
/*                      BuildFullQueryStatement()                       */
/************************************************************************/

void OGROCITableLayer::BuildFullQueryStatement()

{
    if (pszQueryStatement != nullptr)
    {
        CPLFree(pszQueryStatement);
        pszQueryStatement = nullptr;
    }

    OGROCIStringBuf oCmd;
    char *pszFields = BuildFields();

    oCmd.Append("SELECT ");
    oCmd.Append(pszFields);
    oCmd.Append(" FROM ");
    oCmd.Append(poFeatureDefn->GetName());
    oCmd.Append(" ");
    oCmd.Append(pszWHERE);

    pszQueryStatement = oCmd.StealString();

    CPLFree(pszFields);
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGROCITableLayer::GetFeature(GIntBig nFeatureId)

{

    /* -------------------------------------------------------------------- */
    /*      If we don't have an FID column scan for the desired feature.    */
    /* -------------------------------------------------------------------- */
    if (pszFIDName == nullptr)
        return OGROCILayer::GetFeature(nFeatureId);

    /* -------------------------------------------------------------------- */
    /*      Clear any existing query.                                       */
    /* -------------------------------------------------------------------- */
    ResetReading();

    /* -------------------------------------------------------------------- */
    /*      Build query for this specific feature.                          */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf oCmd;
    char *pszFields = BuildFields();

    oCmd.Append("SELECT ");
    oCmd.Append(pszFields);
    oCmd.Append(" FROM ");
    oCmd.Append(poFeatureDefn->GetName());
    oCmd.Append(" ");
    oCmd.Appendf(static_cast<int>(50 + strlen(pszFIDName)),
                 " WHERE \"%s\" = " CPL_FRMT_GIB " ", pszFIDName, nFeatureId);

    CPLFree(pszFields);

    /* -------------------------------------------------------------------- */
    /*      Execute the statement.                                          */
    /* -------------------------------------------------------------------- */
    if (!ExecuteQuery(oCmd.GetString()))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Get the feature.                                                */
    /* -------------------------------------------------------------------- */
    OGRFeature *poFeature;

    poFeature = GetNextRawFeature();

    if (poFeature != nullptr && poFeature->GetGeometryRef() != nullptr)
        poFeature->GetGeometryRef()->assignSpatialReference(poSRS);

    /* -------------------------------------------------------------------- */
    /*      Cleanup the statement.                                          */
    /* -------------------------------------------------------------------- */
    ResetReading();

    /* -------------------------------------------------------------------- */
    /*      verify the FID.                                                 */
    /* -------------------------------------------------------------------- */
    if (poFeature != nullptr && poFeature->GetFID() != nFeatureId)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGROCITableLayer::GetFeature(" CPL_FRMT_GIB
                 ") ... query returned feature " CPL_FRMT_GIB " instead!",
                 nFeatureId, poFeature->GetFID());
        delete poFeature;
        return nullptr;
    }
    else
        return poFeature;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/*                                                                      */
/*      We override the next feature method because we know that we     */
/*      implement the attribute query within the statement and so we    */
/*      don't have to test here.   Eventually the spatial query will    */
/*      be fully tested within the statement as well.                   */
/************************************************************************/

OGRFeature *OGROCITableLayer::GetNextFeature()

{

    while (true)
    {
        OGRFeature *poFeature;

        poFeature = GetNextRawFeature();
        if (poFeature == nullptr)
        {
            CPLDebug("OCI", "Query complete, got %d hits, and %d discards.",
                     nHits, nDiscarded);
            nHits = 0;
            nDiscarded = 0;
            return nullptr;
        }

        if (m_poFilterGeom == nullptr ||
            FilterGeometry(poFeature->GetGeometryRef()))
        {
            nHits++;
            if (poFeature->GetGeometryRef() != nullptr)
                poFeature->GetGeometryRef()->assignSpatialReference(poSRS);
            return poFeature;
        }

        if (m_poFilterGeom != nullptr)
            nDiscarded++;

        delete poFeature;
    }
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGROCITableLayer::ResetReading()

{
    nHits = 0;
    nDiscarded = 0;

    FlushPendingFeatures();

    BuildFullQueryStatement();

    OGROCILayer::ResetReading();
}

/************************************************************************/
/*                            BuildFields()                             */
/*                                                                      */
/*      Build list of fields to fetch, performing any required          */
/*      transformations (such as on geometry).                          */
/************************************************************************/

char *OGROCITableLayer::BuildFields()

{
    int i;
    OGROCIStringBuf oFldList;

    if (pszGeomName)
    {
        oFldList.Append("\"");
        oFldList.Append(pszGeomName);
        oFldList.Append("\"");
        iGeomColumn = 0;
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        const char *pszName = poFeatureDefn->GetFieldDefn(i)->GetNameRef();

        if (oFldList.GetLast() != '\0')
            oFldList.Append(",");

        oFldList.Append("\"");
        oFldList.Append(pszName);
        oFldList.Append("\"");
    }

    if (pszFIDName != nullptr)
    {
        iFIDColumn = poFeatureDefn->GetFieldCount();
        oFldList.Append(",\"");
        oFldList.Append(pszFIDName);
        oFldList.Append("\"");
    }

    return oFldList.StealString();
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGROCITableLayer::SetAttributeFilter(const char *pszQueryIn)

{
    CPLFree(m_pszAttrQueryString);
    m_pszAttrQueryString = (pszQueryIn) ? CPLStrdup(pszQueryIn) : nullptr;

    if ((pszQueryIn == nullptr && this->pszQuery == nullptr) ||
        (pszQueryIn != nullptr && this->pszQuery != nullptr &&
         strcmp(pszQueryIn, this->pszQuery) == 0))
        return OGRERR_NONE;

    CPLFree(this->pszQuery);

    if (pszQueryIn == nullptr)
        this->pszQuery = nullptr;
    else
        this->pszQuery = CPLStrdup(pszQueryIn);

    BuildWhere();

    ResetReading();

    return OGRERR_NONE;
}

/************************************************************************/
/*                             ISetFeature()                             */
/*                                                                      */
/*      We implement SetFeature() by deleting the existing row (if      */
/*      it exists), and then using CreateFeature() to write it out      */
/*      tot he table normally.  CreateFeature() will preserve the       */
/*      existing FID if possible.                                       */
/************************************************************************/

OGRErr OGROCITableLayer::ISetFeature(OGRFeature *poFeature)

{
    /* -------------------------------------------------------------------- */
    /*      Do some validation.                                             */
    /* -------------------------------------------------------------------- */
    if (pszFIDName == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGROCITableLayer::ISetFeature(" CPL_FRMT_GIB
                 ") failed because there is "
                 "no apparent FID column on table %s.",
                 poFeature->GetFID(), poFeatureDefn->GetName());

        return OGRERR_FAILURE;
    }

    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGROCITableLayer::ISetFeature(" CPL_FRMT_GIB
                 ") failed because the feature "
                 "has no FID!",
                 poFeature->GetFID());

        return OGRERR_FAILURE;
    }

    OGRErr eErr = DeleteFeature(poFeature->GetFID());
    if (eErr != OGRERR_NONE)
        return eErr;

    return CreateFeature(poFeature);
}

/************************************************************************/
/*                           DeleteFeature()                            */
/************************************************************************/

OGRErr OGROCITableLayer::DeleteFeature(GIntBig nFID)

{
    /* -------------------------------------------------------------------- */
    /*      Do some validation.                                             */
    /* -------------------------------------------------------------------- */
    if (pszFIDName == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGROCITableLayer::DeleteFeature(" CPL_FRMT_GIB
                 ") failed because there is "
                 "no apparent FID column on table %s.",
                 nFID, poFeatureDefn->GetName());

        return OGRERR_FAILURE;
    }

    if (nFID == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGROCITableLayer::DeleteFeature(" CPL_FRMT_GIB
                 ") failed for Null FID",
                 nFID);

        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Prepare the delete command, and execute.  We don't check the    */
    /*      error result of the execute, since attempting to Set a          */
    /*      non-existing feature may be OK.                                 */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf oCmdText;
    OGROCIStatement oCmdStatement(poDS->GetSession());

    oCmdText.Appendf(static_cast<int>(strlen(poFeatureDefn->GetName()) +
                                      strlen(pszFIDName) + 100),
                     "DELETE FROM %s WHERE \"%s\" = " CPL_FRMT_GIB,
                     poFeatureDefn->GetName(), pszFIDName, nFID);

    if (oCmdStatement.Execute(oCmdText.GetString()) == CE_None)
        return (oCmdStatement.GetAffectedRows() > 0)
                   ? OGRERR_NONE
                   : OGRERR_NON_EXISTING_FEATURE;
    else
        return OGRERR_FAILURE;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGROCITableLayer::ICreateFeature(OGRFeature *poFeature)

{
    /* -------------------------------------------------------------------- */
    /*      Add extents of this geometry to the existing layer extents.     */
    /* -------------------------------------------------------------------- */
    if (poFeature->GetGeometryRef() != nullptr)
    {
        OGREnvelope sThisExtent;

        poFeature->GetGeometryRef()->getEnvelope(&sThisExtent);

        if (!sExtent.Contains(sThisExtent))
        {
            sExtent.Merge(sThisExtent);
            bExtentUpdated = true;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Get the first id value from open options                        */
    /* -------------------------------------------------------------------- */

    this->nFirstId = -1;

    if (CSLFetchNameValue(papszOptions, "FIRST_ID") != nullptr)
    {
        this->nFirstId = atoi(CSLFetchNameValue(papszOptions, "FIRST_ID"));
    }

    /* -------------------------------------------------------------------- */
    /*      Get the multi load count value from open options                */
    /* -------------------------------------------------------------------- */

    this->bMultiLoad = CPLFetchBool(papszOptions, "MULTI_LOAD", true);

    this->nMultiLoadCount = 100;

    if (CSLFetchNameValue(papszOptions, "MULTI_LOAD_COUNT") != nullptr)
    {
        this->nMultiLoadCount =
            atoi(CSLFetchNameValue(papszOptions, "MULTI_LOAD_COUNT"));
        this->bMultiLoad = true;  // overwrites MULTI_LOAD=NO
    }

    /* -------------------------------------------------------------------- */
    /*      Do the actual creation.                                         */
    /* -------------------------------------------------------------------- */
    if (bMultiLoad)
        return BoundCreateFeature(poFeature);
    else
        return UnboundCreateFeature(poFeature);
}

/************************************************************************/
/*                        UnboundCreateFeature()                        */
/************************************************************************/

OGRErr OGROCITableLayer::UnboundCreateFeature(OGRFeature *poFeature)

{
    OGROCISession *poSession = poDS->GetSession();
    char *pszCommand;
    int bNeedComma = FALSE;
    size_t nCommandBufSize;

    /* -------------------------------------------------------------------- */
    /*      Prepare SQL statement buffer.                                   */
    /* -------------------------------------------------------------------- */
    nCommandBufSize = 2000;
    pszCommand = (char *)CPLMalloc(nCommandBufSize);

    /* -------------------------------------------------------------------- */
    /*      Form the INSERT command.                                        */
    /* -------------------------------------------------------------------- */
    snprintf(pszCommand, nCommandBufSize, "INSERT INTO \"%s\"(\"",
             poFeatureDefn->GetName());

    if (poFeature->GetGeometryRef() != nullptr)
    {
        bNeedComma = TRUE;
        strcat(pszCommand, pszGeomName);
    }

    if (pszFIDName != nullptr)
    {
        if (bNeedComma)
            strcat(pszCommand, "\",\"");

        strcat(pszCommand, pszFIDName);
        bNeedComma = TRUE;
    }

    for (int i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (!poFeature->IsFieldSetAndNotNull(i))
            continue;

        if (!bNeedComma)
            bNeedComma = TRUE;
        else
            strcat(pszCommand, "\",\"");

        snprintf(pszCommand + strlen(pszCommand),
                 nCommandBufSize - strlen(pszCommand), "%s",
                 poFeatureDefn->GetFieldDefn(i)->GetNameRef());
    }

    strcat(pszCommand, "\") VALUES (");

    CPLAssert(strlen(pszCommand) < nCommandBufSize);

    /* -------------------------------------------------------------------- */
    /*      Set the geometry                                                */
    /* -------------------------------------------------------------------- */
    bNeedComma = poFeature->GetGeometryRef() != nullptr;
    if (poFeature->GetGeometryRef() != nullptr)
    {
        OGRGeometry *poGeometry = poFeature->GetGeometryRef();
        char szSDO_GEOMETRY[512];
        char szSRID[128];

        if (nSRID == -1)
            strcpy(szSRID, "NULL");
        else
            snprintf(szSRID, sizeof(szSRID), "%d", nSRID);

        if (wkbFlatten(poGeometry->getGeometryType()) == wkbPoint)
        {
            OGRPoint *poPoint = poGeometry->toPoint();

            if (nDimension == 2)
                CPLsnprintf(
                    szSDO_GEOMETRY, sizeof(szSDO_GEOMETRY),
                    "%s(%d,%s,MDSYS.SDO_POINT_TYPE(%.16g,%.16g,0),NULL,NULL)",
                    SDO_GEOMETRY, 2001, szSRID, poPoint->getX(),
                    poPoint->getY());
            else
                CPLsnprintf(szSDO_GEOMETRY, sizeof(szSDO_GEOMETRY),
                            "%s(%d,%s,MDSYS.SDO_POINT_TYPE(%.16g,%.16g,%.16g),"
                            "NULL,NULL)",
                            SDO_GEOMETRY, 3001, szSRID, poPoint->getX(),
                            poPoint->getY(), poPoint->getZ());
        }
        else
        {
            int nGType;

            if (TranslateToSDOGeometry(poFeature->GetGeometryRef(), &nGType) ==
                OGRERR_NONE)
                CPLsnprintf(szSDO_GEOMETRY, sizeof(szSDO_GEOMETRY),
                            "%s(%d,%s,NULL,:elem_info,:ordinates)",
                            SDO_GEOMETRY, nGType, szSRID);
            else
                CPLsnprintf(szSDO_GEOMETRY, sizeof(szSDO_GEOMETRY), "NULL");
        }

        if (strlen(pszCommand) + strlen(szSDO_GEOMETRY) > nCommandBufSize - 50)
        {
            nCommandBufSize =
                strlen(pszCommand) + strlen(szSDO_GEOMETRY) + 10000;
            pszCommand = (char *)CPLRealloc(pszCommand, nCommandBufSize);
        }

        strcat(pszCommand, szSDO_GEOMETRY);
    }

    /* -------------------------------------------------------------------- */
    /*      Set the FID.                                                    */
    /* -------------------------------------------------------------------- */
    size_t nOffset = strlen(pszCommand);

    if (pszFIDName != nullptr)
    {
        GIntBig nFID;

        if (bNeedComma)
            strcat(pszCommand + nOffset, ", ");
        bNeedComma = TRUE;

        nOffset += strlen(pszCommand + nOffset);

        nFID = poFeature->GetFID();
        if (nFID == OGRNullFID)
        {
            if (iNextFIDToWrite < 0)
            {
                iNextFIDToWrite = GetMaxFID() + 1;
            }
            nFID = iNextFIDToWrite++;
            poFeature->SetFID(nFID);
        }
        snprintf(pszCommand + nOffset, nCommandBufSize - nOffset, CPL_FRMT_GIB,
                 nFID);
    }

    /* -------------------------------------------------------------------- */
    /*      Set the other fields.                                           */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (!poFeature->IsFieldSetAndNotNull(i))
            continue;

        OGRFieldDefn *poFldDefn = poFeatureDefn->GetFieldDefn(i);
        const char *pszStrValue = poFeature->GetFieldAsString(i);

        if (bNeedComma)
            strcat(pszCommand + nOffset, ", ");
        else
            bNeedComma = TRUE;

        if (strlen(pszStrValue) + strlen(pszCommand + nOffset) + nOffset >
            nCommandBufSize - 50)
        {
            nCommandBufSize = strlen(pszCommand) + strlen(pszStrValue) + 10000;
            pszCommand = (char *)CPLRealloc(pszCommand, nCommandBufSize);
        }

        if (poFldDefn->GetType() == OFTInteger ||
            poFldDefn->GetType() == OFTInteger64 ||
            poFldDefn->GetType() == OFTReal)
        {
            if (poFldDefn->GetWidth() > 0 && bPreservePrecision &&
                (int)strlen(pszStrValue) > poFldDefn->GetWidth())
            {
                strcat(pszCommand + nOffset, "NULL");
                ReportTruncation(poFldDefn);
            }
            else
                strcat(pszCommand + nOffset, pszStrValue);
        }
        else
        {
            int iChar;

            /* We need to quote and escape string fields. */
            strcat(pszCommand + nOffset, "'");

            nOffset += strlen(pszCommand + nOffset);

            for (iChar = 0; pszStrValue[iChar] != '\0'; iChar++)
            {
                if (poFldDefn->GetWidth() != 0 && bPreservePrecision &&
                    iChar >= poFldDefn->GetWidth())
                {
                    ReportTruncation(poFldDefn);
                    break;
                }

                if (pszStrValue[iChar] == '\'')
                {
                    pszCommand[nOffset++] = '\'';
                    pszCommand[nOffset++] = pszStrValue[iChar];
                }
                else
                    pszCommand[nOffset++] = pszStrValue[iChar];
            }
            pszCommand[nOffset] = '\0';

            strcat(pszCommand + nOffset, "'");
        }
        nOffset += strlen(pszCommand + nOffset);
    }

    strcat(pszCommand + nOffset, ")");

    /* -------------------------------------------------------------------- */
    /*      Prepare statement.                                              */
    /* -------------------------------------------------------------------- */
    OGROCIStatement oInsert(poSession);
    int bHaveOrdinates = strstr(pszCommand, ":ordinates") != nullptr;
    int bHaveElemInfo = strstr(pszCommand, ":elem_info") != nullptr;

    if (oInsert.Prepare(pszCommand) != CE_None)
    {
        CPLFree(pszCommand);
        return OGRERR_FAILURE;
    }

    CPLFree(pszCommand);

    /* -------------------------------------------------------------------- */
    /*      Bind and translate the elem_info if we have some.               */
    /* -------------------------------------------------------------------- */
    if (bHaveElemInfo)
    {
        OCIBind *hBindOrd = nullptr;
        int i;
        OCINumber oci_number;

        // Create or clear VARRAY
        if (hElemInfoVARRAY == nullptr)
        {
            if (poSession->Failed(
                    OCIObjectNew(poSession->hEnv, poSession->hError,
                                 poSession->hSvcCtx, OCI_TYPECODE_VARRAY,
                                 poSession->hElemInfoTDO, nullptr,
                                 OCI_DURATION_SESSION, FALSE,
                                 reinterpret_cast<dvoid **>(&hElemInfoVARRAY)),
                    "OCIObjectNew(hElemInfoVARRAY)"))
                return OGRERR_FAILURE;
        }
        else
        {
            sb4 nOldCount;

            OCICollSize(poSession->hEnv, poSession->hError, hElemInfoVARRAY,
                        &nOldCount);
            OCICollTrim(poSession->hEnv, poSession->hError, nOldCount,
                        hElemInfoVARRAY);
        }

        // Prepare the VARRAY of ordinate values.
        for (i = 0; i < nElemInfoCount; i++)
        {
            if (poSession->Failed(
                    OCINumberFromInt(poSession->hError,
                                     static_cast<dvoid *>(panElemInfo + i),
                                     (uword)sizeof(int), OCI_NUMBER_SIGNED,
                                     &oci_number),
                    "OCINumberFromInt"))
                return OGRERR_FAILURE;

            if (poSession->Failed(
                    OCICollAppend(poSession->hEnv, poSession->hError,
                                  static_cast<dvoid *>(&oci_number), nullptr,
                                  hElemInfoVARRAY),
                    "OCICollAppend"))
                return OGRERR_FAILURE;
        }

        // Do the binding.
        if (poSession->Failed(
                OCIBindByName(
                    oInsert.GetStatement(), &hBindOrd, poSession->hError,
                    reinterpret_cast<text *>(const_cast<char *>(":elem_info")),
                    (sb4)-1, nullptr, 0, SQLT_NTY, nullptr, nullptr, nullptr,
                    (ub4)0, nullptr, (ub4)OCI_DEFAULT),
                "OCIBindByName(:elem_info)"))
            return OGRERR_FAILURE;

        if (poSession->Failed(
                OCIBindObject(hBindOrd, poSession->hError,
                              poSession->hElemInfoTDO,
                              reinterpret_cast<dvoid **>(&hElemInfoVARRAY),
                              nullptr, nullptr, nullptr),
                "OCIBindObject(:elem_info)"))
            return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Bind and translate the ordinates if we have some.               */
    /* -------------------------------------------------------------------- */
    if (bHaveOrdinates)
    {
        OCIBind *hBindOrd = nullptr;
        int i;
        OCINumber oci_number;

        // Create or clear VARRAY
        if (hOrdVARRAY == nullptr)
        {
            if (poSession->Failed(
                    OCIObjectNew(poSession->hEnv, poSession->hError,
                                 poSession->hSvcCtx, OCI_TYPECODE_VARRAY,
                                 poSession->hOrdinatesTDO, nullptr,
                                 OCI_DURATION_SESSION, FALSE,
                                 reinterpret_cast<dvoid **>(&hOrdVARRAY)),
                    "OCIObjectNew(hOrdVARRAY)"))
                return OGRERR_FAILURE;
        }
        else
        {
            sb4 nOldCount;

            OCICollSize(poSession->hEnv, poSession->hError, hOrdVARRAY,
                        &nOldCount);
            OCICollTrim(poSession->hEnv, poSession->hError, nOldCount,
                        hOrdVARRAY);
        }

        // Prepare the VARRAY of ordinate values.
        for (i = 0; i < nOrdinalCount; i++)
        {
            if (poSession->Failed(
                    OCINumberFromReal(poSession->hError,
                                      static_cast<dvoid *>(padfOrdinals + i),
                                      (uword)sizeof(double), &oci_number),
                    "OCINumberFromReal"))
                return OGRERR_FAILURE;

            if (poSession->Failed(
                    OCICollAppend(poSession->hEnv, poSession->hError,
                                  (dvoid *)&oci_number, nullptr, hOrdVARRAY),
                    "OCICollAppend"))
                return OGRERR_FAILURE;
        }

        // Do the binding.
        if (poSession->Failed(
                OCIBindByName(
                    oInsert.GetStatement(), &hBindOrd, poSession->hError,
                    reinterpret_cast<text *>(const_cast<char *>(":ordinates")),
                    (sb4)-1, nullptr, 0, SQLT_NTY, nullptr, nullptr, nullptr, 0,
                    nullptr, (ub4)OCI_DEFAULT),
                "OCIBindByName(:ordinates)"))
            return OGRERR_FAILURE;

        if (poSession->Failed(
                OCIBindObject(hBindOrd, poSession->hError,
                              poSession->hOrdinatesTDO,
                              reinterpret_cast<dvoid **>(&hOrdVARRAY), nullptr,
                              nullptr, nullptr),
                "OCIBindObject(:ordinates)"))
            return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Execute the insert.                                             */
    /* -------------------------------------------------------------------- */
    if (oInsert.Execute(nullptr) != CE_None)
        return OGRERR_FAILURE;
    else
        return OGRERR_NONE;
}

/************************************************************************/
/*                           IGetExtent()                               */
/************************************************************************/

OGRErr OGROCITableLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                                    bool bForce)

{
    CPLAssert(nullptr != psExtent);

    OGRErr err = OGRERR_FAILURE;

    if (EQUAL(GetGeometryColumn(), ""))
    {
        return OGRERR_NONE;
    }

    /* -------------------------------------------------------------------- */
    /*      Build query command.                                        */
    /* -------------------------------------------------------------------- */
    CPLAssert(nullptr != pszGeomName);

    OGROCIStringBuf oCommand;
    oCommand.Appendf(
        1000,
        "SELECT "
        "MIN(SDO_GEOM.SDO_MIN_MBR_ORDINATE(t.%s,m.DIMINFO,1)) AS MINX,"
        "MIN(SDO_GEOM.SDO_MIN_MBR_ORDINATE(t.%s,m.DIMINFO,2)) AS MINY,"
        "MAX(SDO_GEOM.SDO_MAX_MBR_ORDINATE(t.%s,m.DIMINFO,1)) AS MAXX,"
        "MAX(SDO_GEOM.SDO_MAX_MBR_ORDINATE(t.%s,m.DIMINFO,2)) AS MAXY "
        "FROM ALL_SDO_GEOM_METADATA m, ",
        pszGeomName, pszGeomName, pszGeomName, pszGeomName);

    if (osOwner != "")
    {
        oCommand.Appendf(500, " %s.%s t ", osOwner.c_str(),
                         osTableName.c_str());
    }
    else
    {
        oCommand.Appendf(500, " %s t ", osTableName.c_str());
    }

    oCommand.Appendf(
        500, "WHERE m.TABLE_NAME = UPPER('%s') AND m.COLUMN_NAME = UPPER('%s')",
        osTableName.c_str(), pszGeomName);

    if (osOwner != "")
    {
        oCommand.Appendf(500, " AND OWNER = UPPER('%s')", osOwner.c_str());
    }

    /* -------------------------------------------------------------------- */
    /*      Execute query command.                                          */
    /* -------------------------------------------------------------------- */
    OGROCISession *poSession = poDS->GetSession();
    CPLAssert(nullptr != poSession);

    OGROCIStatement oGetExtent(poSession);

    if (oGetExtent.Execute(oCommand.GetString()) == CE_None)
    {
        char **papszRow = oGetExtent.SimpleFetchRow();

        if (papszRow != nullptr && papszRow[0] != nullptr &&
            papszRow[1] != nullptr && papszRow[2] != nullptr &&
            papszRow[3] != nullptr)
        {
            psExtent->MinX = CPLAtof(papszRow[0]);
            psExtent->MinY = CPLAtof(papszRow[1]);
            psExtent->MaxX = CPLAtof(papszRow[2]);
            psExtent->MaxY = CPLAtof(papszRow[3]);

            err = OGRERR_NONE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Query spatial extent of layer using default,                    */
    /*      but not optimized implementation.                               */
    /* -------------------------------------------------------------------- */
    if (err != OGRERR_NONE)
    {
        err = OGRLayer::IGetExtent(iGeomField, psExtent, bForce);
        CPLDebug("OCI", "Failing to query extent of %s using default GetExtent",
                 osTableName.c_str());
    }

    return err;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGROCITableLayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCSequentialWrite) || EQUAL(pszCap, OLCRandomWrite))
        return bUpdateAccess;

    else if (EQUAL(pszCap, OLCCreateField))
        return bUpdateAccess;

    else
        return OGROCILayer::TestCapability(pszCap);
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/*                                                                      */
/*      If a spatial filter is in effect, we turn control over to       */
/*      the generic counter.  Otherwise we return the total count.      */
/*      Eventually we should consider implementing a more efficient     */
/*      way of counting features matching a spatial query.              */
/************************************************************************/

GIntBig OGROCITableLayer::GetFeatureCount(int bForce)

{
    /* -------------------------------------------------------------------- */
    /*      Use a more brute force mechanism if we have a spatial query     */
    /*      in play.                                                        */
    /* -------------------------------------------------------------------- */
    if (m_poFilterGeom != nullptr)
        return OGROCILayer::GetFeatureCount(bForce);

    /* -------------------------------------------------------------------- */
    /*      In theory it might be wise to cache this result, but it         */
    /*      won't be trivial to work out the lifetime of the value.         */
    /*      After all someone else could be adding records from another     */
    /*      application when working against a database.                    */
    /* -------------------------------------------------------------------- */
    OGROCISession *poSession = poDS->GetSession();
    OGROCIStatement oGetCount(poSession);
    char szCommand[1024];
    char **papszResult;

    snprintf(szCommand, sizeof(szCommand), "SELECT COUNT(*) FROM %s %s",
             poFeatureDefn->GetName(), pszWHERE);

    oGetCount.Execute(szCommand);

    papszResult = oGetCount.SimpleFetchRow();

    if (CSLCount(papszResult) < 1)
    {
        CPLDebug("OCI", "Fast get count failed, doing hard way.");
        return OGROCILayer::GetFeatureCount(bForce);
    }

    return CPLAtoGIntBig(papszResult[0]);
}

/************************************************************************/
/*                         UpdateLayerExtents()                         */
/************************************************************************/

void OGROCITableLayer::UpdateLayerExtents()

{
    if (!bExtentUpdated)
        return;

    bExtentUpdated = false;

    /* -------------------------------------------------------------------- */
    /*      Do we have existing layer extents we need to merge in to the    */
    /*      ones we collected as we created features?                       */
    /* -------------------------------------------------------------------- */
    bool bHaveOldExtent = false;

    if (!bNewLayer && pszGeomName)
    {
        OGROCIStringBuf oCommand;

        oCommand.Appendf(
            1000,
            "select min(case when r=1 then sdo_lb else null end) minx, "
            "min(case when r=2 then sdo_lb else null end) miny, "
            "min(case when r=1 then sdo_ub else null end) maxx, min(case when "
            "r=2 then sdo_ub else null end) maxy"
            " from (SELECT d.sdo_dimname, d.sdo_lb, sdo_ub, sdo_tolerance, "
            "rownum r"
            " FROM ALL_SDO_GEOM_METADATA m, table(m.diminfo) d"
            " where m.table_name = UPPER('%s') and m.COLUMN_NAME = UPPER('%s')",
            osTableName.c_str(), pszGeomName);

        if (osOwner != "")
        {
            oCommand.Appendf(500, " AND OWNER = UPPER('%s')", osOwner.c_str());
        }

        oCommand.Append(" ) ");

        OGROCISession *poSession = poDS->GetSession();
        CPLAssert(nullptr != poSession);

        OGROCIStatement oGetExtent(poSession);

        if (oGetExtent.Execute(oCommand.GetString()) == CE_None)
        {
            char **papszRow = oGetExtent.SimpleFetchRow();

            if (papszRow != nullptr && papszRow[0] != nullptr &&
                papszRow[1] != nullptr && papszRow[2] != nullptr &&
                papszRow[3] != nullptr)
            {
                OGREnvelope sOldExtent;

                bHaveOldExtent = true;

                sOldExtent.MinX = CPLAtof(papszRow[0]);
                sOldExtent.MinY = CPLAtof(papszRow[1]);
                sOldExtent.MaxX = CPLAtof(papszRow[2]);
                sOldExtent.MaxY = CPLAtof(papszRow[3]);

                if (sOldExtent.Contains(sExtent))
                {
                    // nothing to do!
                    sExtent = sOldExtent;
                    bExtentUpdated = false;
                    return;
                }
                else
                {
                    sExtent.Merge(sOldExtent);
                }
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Establish the extents and resolution to use.                    */
    /* -------------------------------------------------------------------- */
    double dfResSize;
    double dfXMin, dfXMax, dfXRes;
    double dfYMin, dfYMax, dfYRes;
    double dfZMin, dfZMax, dfZRes;

    if (sExtent.MaxX - sExtent.MinX > 400)
        dfResSize = 0.001;
    else
        dfResSize = 0.0000001;

    dfXMin = sExtent.MinX - dfResSize * 3;
    dfXMax = sExtent.MaxX + dfResSize * 3;
    dfXRes = dfResSize;
    ParseDIMINFO("DIMINFO_X", &dfXMin, &dfXMax, &dfXRes);

    dfYMin = sExtent.MinY - dfResSize * 3;
    dfYMax = sExtent.MaxY + dfResSize * 3;
    dfYRes = dfResSize;
    ParseDIMINFO("DIMINFO_Y", &dfYMin, &dfYMax, &dfYRes);

    dfZMin = -100000.0;
    dfZMax = 100000.0;
    dfZRes = 0.002;
    ParseDIMINFO("DIMINFO_Z", &dfZMin, &dfZMax, &dfZRes);

    /* -------------------------------------------------------------------- */
    /*      If we already have an extent in the table, we will need to      */
    /*      update it in place.                                             */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf sDimUpdate;

    if (bHaveOldExtent)
    {
        sDimUpdate.Append("UPDATE USER_SDO_GEOM_METADATA ");
        sDimUpdate.Append("SET DIMINFO =");
        sDimUpdate.Append("MDSYS.SDO_DIM_ARRAY(");
        sDimUpdate.Appendf(200, "MDSYS.SDO_DIM_ELEMENT('X',%.16g,%.16g,%.12g)",
                           dfXMin, dfXMax, dfXRes);
        sDimUpdate.Appendf(200, ",MDSYS.SDO_DIM_ELEMENT('Y',%.16g,%.16g,%.12g)",
                           dfYMin, dfYMax, dfYRes);

        if (nDimension == 3)
        {
            sDimUpdate.Appendf(200,
                               ",MDSYS.SDO_DIM_ELEMENT('Z',%.16g,%.16g,%.12g)",
                               dfZMin, dfZMax, dfZRes);
        }

        sDimUpdate.Appendf(
            static_cast<int>(strlen(poFeatureDefn->GetName()) + 100),
            ") WHERE TABLE_NAME = '%s'", poFeatureDefn->GetName());
    }
    else
    {
        /* --------------------------------------------------------------------
         */
        /*      Prepare dimension update statement. */
        /* --------------------------------------------------------------------
         */
        sDimUpdate.Append("INSERT INTO USER_SDO_GEOM_METADATA VALUES ");
        sDimUpdate.Appendf(
            static_cast<int>(strlen(poFeatureDefn->GetName()) + 100),
            "('%s', '%s', ", poFeatureDefn->GetName(), pszGeomName);

        sDimUpdate.Append("MDSYS.SDO_DIM_ARRAY(");
        sDimUpdate.Appendf(200, "MDSYS.SDO_DIM_ELEMENT('X',%.16g,%.16g,%.12g)",
                           dfXMin, dfXMax, dfXRes);
        sDimUpdate.Appendf(200, ",MDSYS.SDO_DIM_ELEMENT('Y',%.16g,%.16g,%.12g)",
                           dfYMin, dfYMax, dfYRes);

        if (nDimension == 3)
        {
            sDimUpdate.Appendf(200,
                               ",MDSYS.SDO_DIM_ELEMENT('Z',%.16g,%.16g,%.12g)",
                               dfZMin, dfZMax, dfZRes);
        }

        if (nSRID == -1)
            sDimUpdate.Append("), NULL)");
        else
            sDimUpdate.Appendf(100, "), %d)", nSRID);
    }

    /* -------------------------------------------------------------------- */
    /*      Run the update/insert command.                                  */
    /* -------------------------------------------------------------------- */
    OGROCIStatement oExecStatement(poDS->GetSession());

    oExecStatement.Execute(sDimUpdate.GetString());
}

/************************************************************************/
/*                   AllocAndBindForWrite()                             */
/************************************************************************/

int OGROCITableLayer::AllocAndBindForWrite()

{
    OGROCISession *poSession = poDS->GetSession();
    int i;

    CPLAssert(nWriteCacheMax == 0);

    /* -------------------------------------------------------------------- */
    /*      Decide on the number of rows we want to be able to cache at     */
    /*      a time.                                                         */
    /* -------------------------------------------------------------------- */
    nWriteCacheMax = nMultiLoadCount;

    /* -------------------------------------------------------------------- */
    /*      Collect the INSERT statement.                                   */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf oCmdBuf;

    oCmdBuf.Append("INSERT /*+ APPEND */ INTO \"");
    oCmdBuf.Append(poFeatureDefn->GetName());
    oCmdBuf.Append("\"(\"");
    oCmdBuf.Append(pszFIDName);

    if (GetGeomType() != wkbNone)
    {
        oCmdBuf.Append("\",\"");
        oCmdBuf.Append(pszGeomName);
    }

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        oCmdBuf.Append("\",\"");
        oCmdBuf.Append(poFeatureDefn->GetFieldDefn(i)->GetNameRef());
    }

    oCmdBuf.Append("\") VALUES ( :fid ");

    if (GetGeomType() != wkbNone)
        oCmdBuf.Append(", :geometry");

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        oCmdBuf.Append(", ");
        oCmdBuf.Appendf(20, " :field_%d", i);
    }

    oCmdBuf.Append(") ");

    /* -------------------------------------------------------------------- */
    /*      Bind and Prepare it.                                            */
    /* -------------------------------------------------------------------- */
    poBoundStatement = new OGROCIStatement(poSession);
    poBoundStatement->Prepare(oCmdBuf.GetString());

    /* -------------------------------------------------------------------- */
    /*      Setup geometry indicator information.                           */
    /* -------------------------------------------------------------------- */
    if (GetGeomType() != wkbNone)
    {
        pasWriteGeomInd = (SDO_GEOMETRY_ind *)CPLCalloc(
            sizeof(SDO_GEOMETRY_ind), nWriteCacheMax);

        papsWriteGeomIndMap = (SDO_GEOMETRY_ind **)CPLCalloc(
            sizeof(SDO_GEOMETRY_ind *), nWriteCacheMax);

        for (i = 0; i < nWriteCacheMax; i++)
            papsWriteGeomIndMap[i] = pasWriteGeomInd + i;

        /* --------------------------------------------------------------------
         */
        /*      Setup all the required geometry objects, and the */
        /*      corresponding indicator map. */
        /* --------------------------------------------------------------------
         */
        pasWriteGeoms = (SDO_GEOMETRY_TYPE *)CPLCalloc(
            sizeof(SDO_GEOMETRY_TYPE), nWriteCacheMax);
        papsWriteGeomMap = (SDO_GEOMETRY_TYPE **)CPLCalloc(
            sizeof(SDO_GEOMETRY_TYPE *), nWriteCacheMax);

        for (i = 0; i < nWriteCacheMax; i++)
            papsWriteGeomMap[i] = pasWriteGeoms + i;

        /* --------------------------------------------------------------------
         */
        /*      Allocate VARRAYs for the elem_info and ordinates. */
        /* --------------------------------------------------------------------
         */
        for (i = 0; i < nWriteCacheMax; i++)
        {
            if (poSession->Failed(
                    OCIObjectNew(poSession->hEnv, poSession->hError,
                                 poSession->hSvcCtx, OCI_TYPECODE_VARRAY,
                                 poSession->hElemInfoTDO, nullptr,
                                 OCI_DURATION_SESSION, FALSE,
                                 reinterpret_cast<dvoid **>(
                                     &(pasWriteGeoms[i].sdo_elem_info))),
                    "OCIObjectNew(elem_info)"))
                return FALSE;

            if (poSession->Failed(
                    OCIObjectNew(poSession->hEnv, poSession->hError,
                                 poSession->hSvcCtx, OCI_TYPECODE_VARRAY,
                                 poSession->hOrdinatesTDO, nullptr,
                                 OCI_DURATION_SESSION, FALSE,
                                 reinterpret_cast<dvoid **>(
                                     &(pasWriteGeoms[i].sdo_ordinates))),
                    "OCIObjectNew(ordinates)"))
                return FALSE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Bind the geometry column. */
        /* --------------------------------------------------------------------
         */
        if (poBoundStatement->BindObject(
                ":geometry", papsWriteGeomMap, poSession->hGeometryTDO,
                (void **)papsWriteGeomIndMap) != CE_None)
            return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Bind the FID column.                                            */
    /* -------------------------------------------------------------------- */
    panWriteFIDs = (int *)CPLMalloc(sizeof(int) * nWriteCacheMax);

    if (poBoundStatement->BindScalar(":fid", panWriteFIDs, sizeof(int),
                                     SQLT_INT) != CE_None)
        return FALSE;

    /* -------------------------------------------------------------------- */
    /*      Allocate each of the column data bind arrays.                   */
    /* -------------------------------------------------------------------- */

    papWriteFields =
        (void **)CPLMalloc(sizeof(void *) * poFeatureDefn->GetFieldCount());
    papaeWriteFieldInd =
        (OCIInd **)CPLCalloc(sizeof(OCIInd *), poFeatureDefn->GetFieldCount());

    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        OGRFieldDefn *poFldDefn = poFeatureDefn->GetFieldDefn(i);
        char szFieldPlaceholderName[80];

        snprintf(szFieldPlaceholderName, sizeof(szFieldPlaceholderName),
                 ":field_%d", i);

        papaeWriteFieldInd[i] =
            (OCIInd *)CPLCalloc(sizeof(OCIInd), nWriteCacheMax);

        if (poFldDefn->GetType() == OFTInteger)
        {
            papWriteFields[i] = (void *)CPLCalloc(sizeof(int), nWriteCacheMax);

            if (poBoundStatement->BindScalar(
                    szFieldPlaceholderName, papWriteFields[i], sizeof(int),
                    SQLT_INT, papaeWriteFieldInd[i]) != CE_None)
                return FALSE;
        }
        else if (poFldDefn->GetType() == OFTInteger64)
        {
            papWriteFields[i] =
                (void *)CPLCalloc(sizeof(GIntBig), nWriteCacheMax);

            if (poBoundStatement->BindScalar(
                    szFieldPlaceholderName, papWriteFields[i], sizeof(GIntBig),
                    SQLT_INT, papaeWriteFieldInd[i]) != CE_None)
                return FALSE;
        }
        else if (poFldDefn->GetType() == OFTReal)
        {
            papWriteFields[i] =
                (void *)CPLCalloc(sizeof(double), nWriteCacheMax);

            if (poBoundStatement->BindScalar(
                    szFieldPlaceholderName, papWriteFields[i], sizeof(double),
                    SQLT_FLT, papaeWriteFieldInd[i]) != CE_None)
                return FALSE;
        }
        else
        {
            int nEachBufSize = nDefaultStringSize + 1;

            if (poFldDefn->GetType() == OFTString && poFldDefn->GetWidth() != 0)
                nEachBufSize = poFldDefn->GetWidth() + 1;

            papWriteFields[i] = (void *)CPLCalloc(nEachBufSize, nWriteCacheMax);

            if (poBoundStatement->BindScalar(
                    szFieldPlaceholderName, papWriteFields[i], nEachBufSize,
                    SQLT_STR, papaeWriteFieldInd[i]) != CE_None)
                return FALSE;
        }
    }

    return TRUE;
}

/************************************************************************/
/*                         BoundCreateFeature()                         */
/************************************************************************/

OGRErr OGROCITableLayer::BoundCreateFeature(OGRFeature *poFeature)

{
    OGROCISession *poSession = poDS->GetSession();
    int iCache, i;
    OGRErr eErr;
    OCINumber oci_number;

    /* If an unset field has a default value, the current implementation */
    /* of BoundCreateFeature() doesn't work. */
    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (!poFeature->IsFieldSetAndNotNull(i) &&
            poFeature->GetFieldDefnRef(i)->GetDefault() != nullptr)
        {
            FlushPendingFeatures();
            return UnboundCreateFeature(poFeature);
        }
    }

    if (!poFeature->Validate(OGR_F_VAL_NULL | OGR_F_VAL_ALLOW_NULL_WHEN_DEFAULT,
                             TRUE))
        return OGRERR_FAILURE;

    iCache = nWriteCacheUsed;

    /* -------------------------------------------------------------------- */
    /*  Initiate the Insert                                                 */
    /* -------------------------------------------------------------------- */
    if (nWriteCacheMax == 0)
    {
        if (!AllocAndBindForWrite())
            return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Set the geometry                                                */
    /* -------------------------------------------------------------------- */
    if (poFeature->GetGeometryRef() != nullptr)
    {
        SDO_GEOMETRY_TYPE *psGeom = pasWriteGeoms + iCache;
        SDO_GEOMETRY_ind *psInd = pasWriteGeomInd + iCache;
        OGRGeometry *poGeometry = poFeature->GetGeometryRef();
        int nGType;

        psInd->_atomic = OCI_IND_NOTNULL;

        if (nSRID == -1)
            psInd->sdo_srid = OCI_IND_NULL;
        else
        {
            psInd->sdo_srid = OCI_IND_NOTNULL;
            OCINumberFromInt(poSession->hError, &nSRID, (uword)sizeof(int),
                             OCI_NUMBER_SIGNED, &(psGeom->sdo_srid));
        }

        /* special more efficient case for simple points */
        if (wkbFlatten(poGeometry->getGeometryType()) == wkbPoint)
        {
            OGRPoint *poPoint = poGeometry->toPoint();
            double dfValue;

            psInd->sdo_point._atomic = OCI_IND_NOTNULL;
            psInd->sdo_elem_info = OCI_IND_NULL;
            psInd->sdo_ordinates = OCI_IND_NULL;

            dfValue = poPoint->getX();
            OCINumberFromReal(poSession->hError, &dfValue,
                              (uword)sizeof(double), &(psGeom->sdo_point.x));

            dfValue = poPoint->getY();
            OCINumberFromReal(poSession->hError, &dfValue,
                              (uword)sizeof(double), &(psGeom->sdo_point.y));

            if (nDimension == 2)
            {
                nGType = 2001;
                psInd->sdo_point.z = OCI_IND_NULL;
            }
            else
            {
                nGType = 3001;
                psInd->sdo_point.z = OCI_IND_NOTNULL;

                dfValue = poPoint->getZ();
                OCINumberFromReal(poSession->hError, &dfValue,
                                  (uword)sizeof(double),
                                  &(psGeom->sdo_point.z));
            }
        }
        else
        {
            psInd->sdo_point._atomic = OCI_IND_NULL;
            psInd->sdo_elem_info = OCI_IND_NOTNULL;
            psInd->sdo_ordinates = OCI_IND_NOTNULL;

            eErr = TranslateToSDOGeometry(poFeature->GetGeometryRef(), &nGType);

            if (eErr != OGRERR_NONE)
                return eErr;

            /* Clear the existing eleminfo and ordinates arrays */
            sb4 nOldCount;

            OCICollSize(poSession->hEnv, poSession->hError,
                        psGeom->sdo_elem_info, &nOldCount);
            OCICollTrim(poSession->hEnv, poSession->hError, nOldCount,
                        psGeom->sdo_elem_info);

            OCICollSize(poSession->hEnv, poSession->hError,
                        psGeom->sdo_ordinates, &nOldCount);
            OCICollTrim(poSession->hEnv, poSession->hError, nOldCount,
                        psGeom->sdo_ordinates);

            // Prepare the VARRAY of element values.
            for (i = 0; i < nElemInfoCount; i++)
            {
                OCINumberFromInt(
                    poSession->hError, static_cast<dvoid *>(panElemInfo + i),
                    (uword)sizeof(int), OCI_NUMBER_SIGNED, &oci_number);

                OCICollAppend(poSession->hEnv, poSession->hError,
                              static_cast<dvoid *>(&oci_number), nullptr,
                              psGeom->sdo_elem_info);
            }

            // Prepare the VARRAY of ordinate values.
            for (i = 0; i < nOrdinalCount; i++)
            {
                OCINumberFromReal(poSession->hError,
                                  static_cast<dvoid *>(padfOrdinals + i),
                                  (uword)sizeof(double), &oci_number);
                OCICollAppend(poSession->hEnv, poSession->hError,
                              static_cast<dvoid *>(&oci_number), nullptr,
                              psGeom->sdo_ordinates);
            }
        }

        psInd->sdo_gtype = OCI_IND_NOTNULL;
        OCINumberFromInt(poSession->hError, &nGType, (uword)sizeof(int),
                         OCI_NUMBER_SIGNED, &(psGeom->sdo_gtype));
    }
    else if (pasWriteGeomInd != nullptr)
    {
        SDO_GEOMETRY_ind *psInd = pasWriteGeomInd + iCache;
        psInd->_atomic = OCI_IND_NULL;
        psInd->sdo_srid = OCI_IND_NULL;
        psInd->sdo_point._atomic = OCI_IND_NULL;
        psInd->sdo_elem_info = OCI_IND_NULL;
        psInd->sdo_ordinates = OCI_IND_NULL;
        psInd->sdo_gtype = OCI_IND_NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Set the FID.                                                    */
    /* -------------------------------------------------------------------- */
    if (poFeature->GetFID() == OGRNullFID)
    {
        if (iNextFIDToWrite < 0)
        {
            iNextFIDToWrite = GetMaxFID() + 1;
        }

        poFeature->SetFID(iNextFIDToWrite++);
    }

    panWriteFIDs[iCache] = static_cast<int>(poFeature->GetFID());

    /* -------------------------------------------------------------------- */
    /*      Set the other fields.                                           */
    /* -------------------------------------------------------------------- */
    for (i = 0; i < poFeatureDefn->GetFieldCount(); i++)
    {
        if (!poFeature->IsFieldSetAndNotNull(i))
        {
            papaeWriteFieldInd[i][iCache] = OCI_IND_NULL;
            continue;
        }

        papaeWriteFieldInd[i][iCache] = OCI_IND_NOTNULL;

        OGRFieldDefn *poFldDefn = poFeatureDefn->GetFieldDefn(i);

        if (poFldDefn->GetType() == OFTInteger)
            ((int *)(papWriteFields[i]))[iCache] =
                poFeature->GetFieldAsInteger(i);

        else if (poFldDefn->GetType() == OFTInteger64)
            ((GIntBig *)(papWriteFields[i]))[iCache] =
                poFeature->GetFieldAsInteger64(i);

        else if (poFldDefn->GetType() == OFTReal)
            ((double *)(papWriteFields[i]))[iCache] =
                poFeature->GetFieldAsDouble(i);

        else
        {
            int nLen = 1;
            int nEachBufSize = nDefaultStringSize + 1;
            const char *pszStrValue = poFeature->GetFieldAsString(i);

            if (poFldDefn->GetType() == OFTString && poFldDefn->GetWidth() != 0)
                nEachBufSize = poFldDefn->GetWidth() + 1;

            nLen = static_cast<int>(strlen(pszStrValue));
            if (nLen > nEachBufSize - 1)
                nLen = nEachBufSize - 1;

            char *pszTarget =
                ((char *)papWriteFields[i]) + iCache * nEachBufSize;
            strncpy(pszTarget, pszStrValue, nLen);
            pszTarget[nLen] = '\0';

            if (poFldDefn->GetType() == OFTDateTime &&
                cpl::contains(setFieldIndexWithTimeStampWithTZ, i))
            {
                const auto *psField = poFeature->GetRawFieldRef(i);
                int nTZHour = 0;
                int nTZMin = 0;
                if (psField->Date.TZFlag > OGR_TZFLAG_MIXED_TZ)
                {
                    const int nOffset =
                        (psField->Date.TZFlag - OGR_TZFLAG_UTC) * 15;
                    nTZHour =
                        static_cast<int>(nOffset / 60);  // Round towards zero.
                    nTZMin = std::abs(nOffset - nTZHour * 60);
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "DateTime %s has no time zone whereas it should "
                             "have. Assuming +00:00",
                             pszTarget);
                }
                CPLsnprintf(pszTarget, nEachBufSize,
                            "%04d-%02d-%02d %02d:%02d:%06.3f %s%02d%02d",
                            psField->Date.Year, psField->Date.Month,
                            psField->Date.Day, psField->Date.Hour,
                            psField->Date.Minute, psField->Date.Second,
                            (psField->Date.TZFlag <= OGR_TZFLAG_MIXED_TZ ||
                             psField->Date.TZFlag >= OGR_TZFLAG_UTC)
                                ? "+"
                                : "-",
                            std::abs(nTZHour), nTZMin);
            }
            else if (poFldDefn->GetType() == OFTDateTime)
            {
                const auto *psField = poFeature->GetRawFieldRef(i);
                if (psField->Date.TZFlag > OGR_TZFLAG_MIXED_TZ)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "DateTime %s has a time zone whereas the target "
                             "field does not support time zone. Time zone will "
                             "be dropped.",
                             pszTarget);
                }
                CPLsnprintf(pszTarget, nEachBufSize,
                            "%04d-%02d-%02d %02d:%02d:%06.3f",
                            psField->Date.Year, psField->Date.Month,
                            psField->Date.Day, psField->Date.Hour,
                            psField->Date.Minute, psField->Date.Second);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Do we need to flush out a full set of rows?                     */
    /* -------------------------------------------------------------------- */
    nWriteCacheUsed++;

    if (nWriteCacheUsed == nWriteCacheMax)
        return FlushPendingFeatures();
    else
        return OGRERR_NONE;
}

/************************************************************************/
/*                        FlushPendingFeatures()                        */
/************************************************************************/

OGRErr OGROCITableLayer::FlushPendingFeatures()

{
    OGROCISession *poSession = poDS->GetSession();

    if (nWriteCacheUsed > 0)
    {
        CPLDebug("OCI", "Flushing %d features on layer %s", nWriteCacheUsed,
                 poFeatureDefn->GetName());

        if (poSession->Failed(
                OCIStmtExecute(poSession->hSvcCtx,
                               poBoundStatement->GetStatement(),
                               poSession->hError, (ub4)nWriteCacheUsed, (ub4)0,
                               (OCISnapshot *)nullptr, (OCISnapshot *)nullptr,
                               (ub4)OCI_COMMIT_ON_SUCCESS),
                "OCIStmtExecute"))
        {
            nWriteCacheUsed = 0;
            return OGRERR_FAILURE;
        }
        else
        {
            nWriteCacheUsed = 0;
            return OGRERR_NONE;
        }
    }
    else
        return OGRERR_NONE;
}

/************************************************************************/
/*                             SyncToDisk()                             */
/*                                                                      */
/*      Perhaps we should also be putting the metadata into a           */
/*      usable state?                                                   */
/************************************************************************/

OGRErr OGROCITableLayer::SyncToDisk()

{
    OGRErr eErr = FlushPendingFeatures();

    UpdateLayerExtents();

    CreateSpatialIndex();

    bNewLayer = FALSE;

    return eErr;
}

/*************************************************************************/
/*                         CreateSpatialIndex()                          */
/*************************************************************************/

void OGROCITableLayer::CreateSpatialIndex()

{
    /* -------------------------------------------------------------------- */
    /*      For new layers we try to create a spatial index.                */
    /* -------------------------------------------------------------------- */
    if (bNewLayer && sExtent.IsInit())
    {
        /* --------------------------------------------------------------------
         */
        /*      If the user has disabled INDEX support then don't create the */
        /*      index. */
        /* --------------------------------------------------------------------
         */
        if (!CPLFetchBool(papszOptions, "SPATIAL_INDEX", true) ||
            !CPLFetchBool(papszOptions, "INDEX", true))
            return;

        /* --------------------------------------------------------------------
         */
        /*      Establish an index name.  For some reason Oracle 8.1.7 does */
        /*      not support spatial index names longer than 18 characters so */
        /*      we magic up an index name if it would be too long. */
        /* --------------------------------------------------------------------
         */
        char szIndexName[20];

        if (strlen(poFeatureDefn->GetName()) < 15)
            snprintf(szIndexName, sizeof(szIndexName), "%s_idx",
                     poFeatureDefn->GetName());
        else if (strlen(poFeatureDefn->GetName()) < 17)
            snprintf(szIndexName, sizeof(szIndexName), "%si",
                     poFeatureDefn->GetName());
        else
        {
            int i, nHash = 0;
            const char *pszSrcName = poFeatureDefn->GetName();

            for (i = 0; pszSrcName[i] != '\0'; i++)
                nHash = (nHash + i * pszSrcName[i]) % 987651;

            snprintf(szIndexName, sizeof(szIndexName), "OSI_%d", nHash);
        }

        poDS->GetSession()->CleanName(szIndexName);

        /* --------------------------------------------------------------------
         */
        /*      Try creating an index on the table now.  Use a simple 5 */
        /*      level quadtree based index.  Would R-tree be a better default?
         */
        /* --------------------------------------------------------------------
         */
        OGROCIStringBuf sIndexCmd;
        OGROCIStatement oExecStatement(poDS->GetSession());

        sIndexCmd.Appendf(10000,
                          "CREATE INDEX \"%s\" ON %s(\"%s\") "
                          "INDEXTYPE IS MDSYS.SPATIAL_INDEX ",
                          szIndexName, poFeatureDefn->GetName(), pszGeomName);

        int bAddLayerGType = CPLTestBool(CSLFetchNameValueDef(
                                 papszOptions, "ADD_LAYER_GTYPE", "YES")) &&
                             GetGeomType() != wkbUnknown;

        CPLString osParams(
            CSLFetchNameValueDef(papszOptions, "INDEX_PARAMETERS", ""));
        if (bAddLayerGType || !osParams.empty())
        {
            sIndexCmd.Append(" PARAMETERS( '");
            if (!osParams.empty())
                sIndexCmd.Append(osParams.c_str());
            if (bAddLayerGType &&
                osParams.ifind("LAYER_GTYPE") == std::string::npos)
            {
                if (!osParams.empty())
                    sIndexCmd.Append(", ");
                sIndexCmd.Append("LAYER_GTYPE=");
                if (wkbFlatten(GetGeomType()) == wkbPoint)
                    sIndexCmd.Append("POINT");
                else if (wkbFlatten(GetGeomType()) == wkbLineString)
                    sIndexCmd.Append("LINE");
                else if (wkbFlatten(GetGeomType()) == wkbPolygon)
                    sIndexCmd.Append("POLYGON");
                else if (wkbFlatten(GetGeomType()) == wkbMultiPoint)
                    sIndexCmd.Append("MULTIPOINT");
                else if (wkbFlatten(GetGeomType()) == wkbMultiLineString)
                    sIndexCmd.Append("MULTILINE");
                else if (wkbFlatten(GetGeomType()) == wkbMultiPolygon)
                    sIndexCmd.Append("MULTIPOLYGON");
                else
                    sIndexCmd.Append("COLLECTION");
            }
            sIndexCmd.Append("' )");
        }

        if (oExecStatement.Execute(sIndexCmd.GetString()) != CE_None)
        {
            CPLString osDropCommand;
            osDropCommand.Printf("DROP INDEX \"%s\"", szIndexName);
            oExecStatement.Execute(osDropCommand);
        }
    }
}

int OGROCITableLayer::GetMaxFID()
{
    if (nFirstId > 0)
        return nFirstId - 1;

    if (pszFIDName == nullptr)
        return 0;

    OGROCIStringBuf sCmd;
    OGROCIStatement oSelect(poDS->GetSession());

    sCmd.Appendf(10000, "SELECT MAX(\"%s\") FROM \"%s\"", pszFIDName,
                 poFeatureDefn->GetName());

    oSelect.Execute(sCmd.GetString());

    char **papszResult = oSelect.SimpleFetchRow();
    return CSLCount(papszResult) == 1 ? atoi(papszResult[0]) : 0;
}
