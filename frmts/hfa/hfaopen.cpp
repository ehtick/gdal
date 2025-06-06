/******************************************************************************
 *
 * Project:  Erdas Imagine (.img) Translator
 * Purpose:  Supporting functions for HFA (.img) ... main (C callable) API
 *           that is not dependent on GDAL (just CPL).
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Intergraph Corporation
 * Copyright (c) 2007-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************
 *
 * hfaopen.cpp
 *
 * Supporting routines for reading Erdas Imagine (.imf) Hierarchical
 * File Architecture files.  This is intended to be a library independent
 * of the GDAL core, but dependent on the Common Portability Library.
 *
 */

#include "cpl_port.h"
#include "hfa_p.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal_priv.h"
#include "hfa.h"
#include "ogr_proj_p.h"
#include "proj.h"

constexpr double R2D = 180.0 / M_PI;

constexpr double RAD2ARCSEC = 648000.0 / M_PI;

static const char *const apszAuxMetadataItems[] = {
    // node/entry            field_name                  metadata_key       type
    "Statistics",
    "dminimum",
    "STATISTICS_MINIMUM",
    "Esta_Statistics",
    "Statistics",
    "dmaximum",
    "STATISTICS_MAXIMUM",
    "Esta_Statistics",
    "Statistics",
    "dmean",
    "STATISTICS_MEAN",
    "Esta_Statistics",
    "Statistics",
    "dmedian",
    "STATISTICS_MEDIAN",
    "Esta_Statistics",
    "Statistics",
    "dmode",
    "STATISTICS_MODE",
    "Esta_Statistics",
    "Statistics",
    "dstddev",
    "STATISTICS_STDDEV",
    "Esta_Statistics",
    "HistogramParameters",
    "lBinFunction.numBins",
    "STATISTICS_HISTONUMBINS",
    "Eimg_StatisticsParameters830",
    "HistogramParameters",
    "dBinFunction.minLimit",
    "STATISTICS_HISTOMIN",
    "Eimg_StatisticsParameters830",
    "HistogramParameters",
    "dBinFunction.maxLimit",
    "STATISTICS_HISTOMAX",
    "Eimg_StatisticsParameters830",
    "StatisticsParameters",
    "lSkipFactorX",
    "STATISTICS_SKIPFACTORX",
    "",
    "StatisticsParameters",
    "lSkipFactorY",
    "STATISTICS_SKIPFACTORY",
    "",
    "StatisticsParameters",
    "dExcludedValues",
    "STATISTICS_EXCLUDEDVALUES",
    "",
    "",
    "elayerType",
    "LAYER_TYPE",
    "",
    "RRDInfoList",
    "salgorithm.string",
    "OVERVIEWS_ALGORITHM",
    "Emif_String",
    nullptr};

const char *const *GetHFAAuxMetaDataList()
{
    return apszAuxMetadataItems;
}

/************************************************************************/
/*                          HFAGetDictionary()                          */
/************************************************************************/

static char *HFAGetDictionary(HFAHandle hHFA)

{
    int nDictMax = 100;
    char *pszDictionary = static_cast<char *>(CPLMalloc(nDictMax));
    int nDictSize = 0;

    if (VSIFSeekL(hHFA->fp, hHFA->nDictionaryPos, SEEK_SET) < 0)
    {
        pszDictionary[nDictSize] = '\0';
        return pszDictionary;
    }

    while (true)
    {
        if (nDictSize >= nDictMax - 1)
        {
            nDictMax = nDictSize * 2 + 100;
            pszDictionary =
                static_cast<char *>(CPLRealloc(pszDictionary, nDictMax));
        }

        if (VSIFReadL(pszDictionary + nDictSize, 1, 1, hHFA->fp) < 1 ||
            pszDictionary[nDictSize] == '\0' ||
            (nDictSize > 2 && pszDictionary[nDictSize - 2] == ',' &&
             pszDictionary[nDictSize - 1] == '.'))
            break;

        nDictSize++;
    }

    pszDictionary[nDictSize] = '\0';

    return pszDictionary;
}

/************************************************************************/
/*                              HFAOpen()                               */
/************************************************************************/

HFAHandle HFAOpen(const char *pszFilename, const char *pszAccess)

{
    VSILFILE *fp = VSIFOpenL(
        pszFilename,
        (EQUAL(pszAccess, "r") || EQUAL(pszAccess, "rb")) ? "rb" : "r+b");

    // Should this be changed to use some sort of CPLFOpen() which will
    // set the error?
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "File open of %s failed.",
                 pszFilename);

        return nullptr;
    }

    // Read and verify the header.
    char szHeader[16] = {};
    if (VSIFReadL(szHeader, 16, 1, fp) < 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to read 16 byte header failed for\n%s.", pszFilename);
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        return nullptr;
    }

    if (!STARTS_WITH_CI(szHeader, "EHFA_HEADER_TAG"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File %s is not an Imagine HFA file ... header wrong.",
                 pszFilename);
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        return nullptr;
    }

    // Create the HFAInfo_t.
    HFAInfo_t *psInfo =
        static_cast<HFAInfo_t *>(CPLCalloc(sizeof(HFAInfo_t), 1));

    psInfo->pszFilename = CPLStrdup(CPLGetFilename(pszFilename));
    psInfo->pszPath = CPLStrdup(CPLGetPathSafe(pszFilename).c_str());
    psInfo->fp = fp;
    if (EQUAL(pszAccess, "r") || EQUAL(pszAccess, "rb"))
        psInfo->eAccess = HFA_ReadOnly;
    else
        psInfo->eAccess = HFA_Update;
    psInfo->bTreeDirty = false;

    // Where is the header?
    GUInt32 nHeaderPos = 0;
    bool bRet = VSIFReadL(&nHeaderPos, sizeof(GInt32), 1, fp) > 0;
    HFAStandard(4, &nHeaderPos);

    // Read the header.
    bRet &= VSIFSeekL(fp, nHeaderPos, SEEK_SET) >= 0;

    bRet &= VSIFReadL(&(psInfo->nVersion), sizeof(GInt32), 1, fp) > 0;
    HFAStandard(4, &(psInfo->nVersion));

    bRet &= VSIFReadL(szHeader, 4, 1, fp) > 0;  // Skip freeList.

    bRet &= VSIFReadL(&(psInfo->nRootPos), sizeof(GInt32), 1, fp) > 0;
    HFAStandard(4, &(psInfo->nRootPos));

    bRet &= VSIFReadL(&(psInfo->nEntryHeaderLength), sizeof(GInt16), 1, fp) > 0;
    HFAStandard(2, &(psInfo->nEntryHeaderLength));

    bRet &= VSIFReadL(&(psInfo->nDictionaryPos), sizeof(GInt32), 1, fp) > 0;
    HFAStandard(4, &(psInfo->nDictionaryPos));

    // Collect file size.
    bRet &= VSIFSeekL(fp, 0, SEEK_END) >= 0;
    if (!bRet)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        CPLFree(psInfo->pszFilename);
        CPLFree(psInfo->pszPath);
        CPLFree(psInfo);
        return nullptr;
    }
    psInfo->nEndOfFile = static_cast<GUInt32>(VSIFTellL(fp));

    // Instantiate the root entry.
    psInfo->poRoot = HFAEntry::New(psInfo, psInfo->nRootPos, nullptr, nullptr);
    if (psInfo->poRoot == nullptr)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        CPLFree(psInfo->pszFilename);
        CPLFree(psInfo->pszPath);
        CPLFree(psInfo);
        return nullptr;
    }

    // Read the dictionary.
    psInfo->pszDictionary = HFAGetDictionary(psInfo);
    psInfo->poDictionary = new HFADictionary(psInfo->pszDictionary);

    // Collect band definitions.
    HFAParseBandInfo(psInfo);

    return psInfo;
}

/************************************************************************/
/*                         HFACreateDependent()                         */
/*                                                                      */
/*      Create a .rrd file for the named file if it does not exist,     */
/*      or return the existing dependent if it already exists.          */
/************************************************************************/

HFAInfo_t *HFACreateDependent(HFAInfo_t *psBase)

{
    if (psBase->psDependent != nullptr)
        return psBase->psDependent;

    // Create desired RRD filename.
    const CPLString oBasename = CPLGetBasenameSafe(psBase->pszFilename);
    const CPLString oRRDFilename =
        CPLFormFilenameSafe(psBase->pszPath, oBasename, "rrd");

    // Does this file already exist?  If so, re-use it.
    VSILFILE *fp = VSIFOpenL(oRRDFilename, "rb");
    if (fp != nullptr)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        psBase->psDependent = HFAOpen(oRRDFilename, "rb");
        // FIXME? this is not going to be reused but recreated...
    }

    // Otherwise create it now.
    HFAInfo_t *psDep = HFACreateLL(oRRDFilename);
    psBase->psDependent = psDep;
    if (psDep == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Add the DependentFile node with the pointer back to the         */
    /*      parent.  When working from an .aux file we really want the      */
    /*      .rrd to point back to the original file, not the .aux file.     */
    /* -------------------------------------------------------------------- */
    HFAEntry *poEntry = psBase->poRoot->GetNamedChild("DependentFile");
    const char *pszDependentFile = nullptr;
    if (poEntry != nullptr)
        pszDependentFile = poEntry->GetStringField("dependent.string");
    if (pszDependentFile == nullptr)
        pszDependentFile = psBase->pszFilename;

    HFAEntry *poDF = HFAEntry::New(psDep, "DependentFile", "Eimg_DependentFile",
                                   psDep->poRoot);

    poDF->MakeData(static_cast<int>(strlen(pszDependentFile) + 50));
    poDF->SetPosition();
    poDF->SetStringField("dependent.string", pszDependentFile);

    return psDep;
}

/************************************************************************/
/*                          HFAGetDependent()                           */
/************************************************************************/

HFAInfo_t *HFAGetDependent(HFAInfo_t *psBase, const char *pszFilename)

{
    if (EQUAL(pszFilename, psBase->pszFilename))
        return psBase;

    if (psBase->psDependent != nullptr)
    {
        if (EQUAL(pszFilename, psBase->psDependent->pszFilename))
            return psBase->psDependent;
        else
            return nullptr;
    }

    // Try to open the dependent file.
    const char *pszMode = psBase->eAccess == HFA_Update ? "r+b" : "rb";

    char *pszDependent = CPLStrdup(
        CPLFormFilenameSafe(psBase->pszPath, pszFilename, nullptr).c_str());

    VSILFILE *fp = VSIFOpenL(pszDependent, pszMode);
    if (fp != nullptr)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        psBase->psDependent = HFAOpen(pszDependent, pszMode);
    }

    CPLFree(pszDependent);

    return psBase->psDependent;
}

/************************************************************************/
/*                          HFAParseBandInfo()                          */
/*                                                                      */
/*      This is used by HFAOpen() and HFACreate() to initialize the     */
/*      band structures.                                                */
/************************************************************************/

CPLErr HFAParseBandInfo(HFAInfo_t *psInfo)

{
    // Find the first band node.
    psInfo->nBands = 0;
    HFAEntry *poNode = psInfo->poRoot->GetChild();
    while (poNode != nullptr)
    {
        if (EQUAL(poNode->GetType(), "Eimg_Layer") &&
            poNode->GetIntField("width") > 0 &&
            poNode->GetIntField("height") > 0)
        {
            if (psInfo->nBands == 0)
            {
                psInfo->nXSize = poNode->GetIntField("width");
                psInfo->nYSize = poNode->GetIntField("height");
            }
            else if (poNode->GetIntField("width") != psInfo->nXSize ||
                     poNode->GetIntField("height") != psInfo->nYSize)
            {
                return CE_Failure;
            }

            psInfo->papoBand = static_cast<HFABand **>(CPLRealloc(
                psInfo->papoBand, sizeof(HFABand *) * (psInfo->nBands + 1)));
            psInfo->papoBand[psInfo->nBands] = new HFABand(psInfo, poNode);
            if (psInfo->papoBand[psInfo->nBands]->nWidth == 0)
            {
                delete psInfo->papoBand[psInfo->nBands];
                return CE_Failure;
            }
            psInfo->nBands++;
        }

        poNode = poNode->GetNext();
    }

    return CE_None;
}

/************************************************************************/
/*                              HFAClose()                              */
/************************************************************************/

int HFAClose(HFAHandle hHFA)

{
    if (hHFA->eAccess == HFA_Update &&
        (hHFA->bTreeDirty || (hHFA->poDictionary != nullptr &&
                              hHFA->poDictionary->bDictionaryTextDirty)))
        HFAFlush(hHFA);

    int nRet = 0;
    if (hHFA->psDependent != nullptr)
    {
        if (HFAClose(hHFA->psDependent) != 0)
            nRet = -1;
    }

    delete hHFA->poRoot;

    if (VSIFCloseL(hHFA->fp) != 0)
        nRet = -1;

    if (hHFA->poDictionary != nullptr)
        delete hHFA->poDictionary;

    CPLFree(hHFA->pszDictionary);
    CPLFree(hHFA->pszFilename);
    CPLFree(hHFA->pszIGEFilename);
    CPLFree(hHFA->pszPath);

    for (int i = 0; i < hHFA->nBands; i++)
    {
        delete hHFA->papoBand[i];
    }

    CPLFree(hHFA->papoBand);

    if (hHFA->pProParameters != nullptr)
    {
        Eprj_ProParameters *psProParams =
            (Eprj_ProParameters *)hHFA->pProParameters;

        CPLFree(psProParams->proExeName);
        CPLFree(psProParams->proName);
        CPLFree(psProParams->proSpheroid.sphereName);

        CPLFree(psProParams);
    }

    if (hHFA->pDatum != nullptr)
    {
        CPLFree(((Eprj_Datum *)hHFA->pDatum)->datumname);
        CPLFree(((Eprj_Datum *)hHFA->pDatum)->gridname);
        CPLFree(hHFA->pDatum);
    }

    if (hHFA->pMapInfo != nullptr)
    {
        CPLFree(((Eprj_MapInfo *)hHFA->pMapInfo)->proName);
        CPLFree(((Eprj_MapInfo *)hHFA->pMapInfo)->units);
        CPLFree(hHFA->pMapInfo);
    }

    CPLFree(hHFA);
    return nRet;
}

/************************************************************************/
/*                              HFARemove()                             */
/*  Used from HFADelete() function.                                     */
/************************************************************************/

static CPLErr HFARemove(const char *pszFilename)

{
    VSIStatBufL sStat;

    if (VSIStatL(pszFilename, &sStat) == 0 && VSI_ISREG(sStat.st_mode))
    {
        if (VSIUnlink(pszFilename) == 0)
            return CE_None;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Attempt to unlink %s failed.", pszFilename);
            return CE_Failure;
        }
    }

    CPLError(CE_Failure, CPLE_AppDefined, "Unable to delete %s, not a file.",
             pszFilename);
    return CE_Failure;
}

/************************************************************************/
/*                              HFADelete()                             */
/************************************************************************/

CPLErr HFADelete(const char *pszFilename)

{
    HFAInfo_t *psInfo = HFAOpen(pszFilename, "rb");
    HFAEntry *poDMS = nullptr;
    HFAEntry *poLayer = nullptr;
    HFAEntry *poNode = nullptr;

    if (psInfo != nullptr)
    {
        poNode = psInfo->poRoot->GetChild();
        while ((poNode != nullptr) && (poLayer == nullptr))
        {
            if (EQUAL(poNode->GetType(), "Eimg_Layer"))
            {
                poLayer = poNode;
            }
            poNode = poNode->GetNext();
        }

        if (poLayer != nullptr)
            poDMS = poLayer->GetNamedChild("ExternalRasterDMS");

        if (poDMS)
        {
            const char *pszRawFilename =
                poDMS->GetStringField("fileName.string");

            if (pszRawFilename != nullptr)
                HFARemove(CPLFormFilenameSafe(psInfo->pszPath, pszRawFilename,
                                              nullptr)
                              .c_str());
        }

        CPL_IGNORE_RET_VAL(HFAClose(psInfo));
    }
    return HFARemove(pszFilename);
}

/************************************************************************/
/*                          HFAGetRasterInfo()                          */
/************************************************************************/

CPLErr HFAGetRasterInfo(HFAHandle hHFA, int *pnXSize, int *pnYSize,
                        int *pnBands)

{
    if (pnXSize != nullptr)
        *pnXSize = hHFA->nXSize;
    if (pnYSize != nullptr)
        *pnYSize = hHFA->nYSize;
    if (pnBands != nullptr)
        *pnBands = hHFA->nBands;
    return CE_None;
}

/************************************************************************/
/*                           HFAGetBandInfo()                           */
/************************************************************************/

CPLErr HFAGetBandInfo(HFAHandle hHFA, int nBand, EPTType *peDataType,
                      int *pnBlockXSize, int *pnBlockYSize,
                      int *pnCompressionType)

{
    if (nBand < 0 || nBand > hHFA->nBands)
    {
        CPLAssert(false);
        return CE_Failure;
    }

    HFABand *poBand = hHFA->papoBand[nBand - 1];

    if (peDataType != nullptr)
        *peDataType = poBand->eDataType;

    if (pnBlockXSize != nullptr)
        *pnBlockXSize = poBand->nBlockXSize;

    if (pnBlockYSize != nullptr)
        *pnBlockYSize = poBand->nBlockYSize;

    // Get compression code from RasterDMS.
    if (pnCompressionType != nullptr)
    {
        *pnCompressionType = 0;

        HFAEntry *poDMS = poBand->poNode->GetNamedChild("RasterDMS");

        if (poDMS != nullptr)
            *pnCompressionType = poDMS->GetIntField("compressionType");
    }

    return CE_None;
}

/************************************************************************/
/*                          HFAGetBandNoData()                          */
/*                                                                      */
/*      returns TRUE if value is set, otherwise FALSE.                  */
/************************************************************************/

int HFAGetBandNoData(HFAHandle hHFA, int nBand, double *pdfNoData)

{
    if (nBand < 0 || nBand > hHFA->nBands)
    {
        CPLAssert(false);
        return CE_Failure;
    }

    HFABand *poBand = hHFA->papoBand[nBand - 1];

    if (!poBand->bNoDataSet && poBand->nOverviews > 0)
    {
        poBand = poBand->papoOverviews[0];
        if (poBand == nullptr)
            return FALSE;
    }

    *pdfNoData = poBand->dfNoData;
    return poBand->bNoDataSet;
}

/************************************************************************/
/*                          HFASetBandNoData()                          */
/*                                                                      */
/*      attempts to set a no-data value on the given band               */
/************************************************************************/

CPLErr HFASetBandNoData(HFAHandle hHFA, int nBand, double dfValue)

{
    if (nBand < 0 || nBand > hHFA->nBands)
    {
        CPLAssert(false);
        return CE_Failure;
    }

    HFABand *poBand = hHFA->papoBand[nBand - 1];

    return poBand->SetNoDataValue(dfValue);
}

/************************************************************************/
/*                        HFAGetOverviewCount()                         */
/************************************************************************/

int HFAGetOverviewCount(HFAHandle hHFA, int nBand)

{
    if (nBand < 0 || nBand > hHFA->nBands)
    {
        CPLAssert(false);
        return CE_Failure;
    }

    HFABand *poBand = hHFA->papoBand[nBand - 1];
    poBand->LoadOverviews();

    return poBand->nOverviews;
}

/************************************************************************/
/*                         HFAGetOverviewInfo()                         */
/************************************************************************/

CPLErr HFAGetOverviewInfo(HFAHandle hHFA, int nBand, int iOverview,
                          int *pnXSize, int *pnYSize, int *pnBlockXSize,
                          int *pnBlockYSize, EPTType *peHFADataType)

{
    if (nBand < 0 || nBand > hHFA->nBands)
    {
        CPLAssert(false);
        return CE_Failure;
    }

    HFABand *poBand = hHFA->papoBand[nBand - 1];
    poBand->LoadOverviews();

    if (iOverview < 0 || iOverview >= poBand->nOverviews)
    {
        CPLAssert(false);
        return CE_Failure;
    }
    poBand = poBand->papoOverviews[iOverview];
    if (poBand == nullptr)
    {
        return CE_Failure;
    }

    if (pnXSize != nullptr)
        *pnXSize = poBand->nWidth;

    if (pnYSize != nullptr)
        *pnYSize = poBand->nHeight;

    if (pnBlockXSize != nullptr)
        *pnBlockXSize = poBand->nBlockXSize;

    if (pnBlockYSize != nullptr)
        *pnBlockYSize = poBand->nBlockYSize;

    if (peHFADataType != nullptr)
        *peHFADataType = poBand->eDataType;

    return CE_None;
}

/************************************************************************/
/*                         HFAGetRasterBlock()                          */
/************************************************************************/

CPLErr HFAGetRasterBlock(HFAHandle hHFA, int nBand, int nXBlock, int nYBlock,
                         void *pData)

{
    return HFAGetRasterBlockEx(hHFA, nBand, nXBlock, nYBlock, pData, -1);
}

/************************************************************************/
/*                        HFAGetRasterBlockEx()                         */
/************************************************************************/

CPLErr HFAGetRasterBlockEx(HFAHandle hHFA, int nBand, int nXBlock, int nYBlock,
                           void *pData, int nDataSize)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->GetRasterBlock(nXBlock, nYBlock, pData,
                                                     nDataSize);
}

/************************************************************************/
/*                     HFAGetOverviewRasterBlock()                      */
/************************************************************************/

CPLErr HFAGetOverviewRasterBlock(HFAHandle hHFA, int nBand, int iOverview,
                                 int nXBlock, int nYBlock, void *pData)

{
    return HFAGetOverviewRasterBlockEx(hHFA, nBand, iOverview, nXBlock, nYBlock,
                                       pData, -1);
}

/************************************************************************/
/*                   HFAGetOverviewRasterBlockEx()                      */
/************************************************************************/

CPLErr HFAGetOverviewRasterBlockEx(HFAHandle hHFA, int nBand, int iOverview,
                                   int nXBlock, int nYBlock, void *pData,
                                   int nDataSize)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    if (iOverview < 0 || iOverview >= hHFA->papoBand[nBand - 1]->nOverviews)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->papoOverviews[iOverview]->GetRasterBlock(
        nXBlock, nYBlock, pData, nDataSize);
}

/************************************************************************/
/*                         HFASetRasterBlock()                          */
/************************************************************************/

CPLErr HFASetRasterBlock(HFAHandle hHFA, int nBand, int nXBlock, int nYBlock,
                         void *pData)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->SetRasterBlock(nXBlock, nYBlock, pData);
}

/************************************************************************/
/*                         HFASetRasterBlock()                          */
/************************************************************************/

CPLErr HFASetOverviewRasterBlock(HFAHandle hHFA, int nBand, int iOverview,
                                 int nXBlock, int nYBlock, void *pData)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    if (iOverview < 0 || iOverview >= hHFA->papoBand[nBand - 1]->nOverviews)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->papoOverviews[iOverview]->SetRasterBlock(
        nXBlock, nYBlock, pData);
}

/************************************************************************/
/*                         HFAGetBandName()                             */
/************************************************************************/

const char *HFAGetBandName(HFAHandle hHFA, int nBand)
{
    if (nBand < 1 || nBand > hHFA->nBands)
        return "";

    return hHFA->papoBand[nBand - 1]->GetBandName();
}

/************************************************************************/
/*                         HFASetBandName()                             */
/************************************************************************/

void HFASetBandName(HFAHandle hHFA, int nBand, const char *pszName)
{
    if (nBand < 1 || nBand > hHFA->nBands)
        return;

    hHFA->papoBand[nBand - 1]->SetBandName(pszName);
}

/************************************************************************/
/*                         HFAGetDataTypeBits()                         */
/************************************************************************/

int HFAGetDataTypeBits(EPTType eDataType)

{
    switch (eDataType)
    {
        case EPT_u1:
            return 1;

        case EPT_u2:
            return 2;

        case EPT_u4:
            return 4;

        case EPT_u8:
        case EPT_s8:
            return 8;

        case EPT_u16:
        case EPT_s16:
            return 16;

        case EPT_u32:
        case EPT_s32:
        case EPT_f32:
            return 32;

        case EPT_f64:
        case EPT_c64:
            return 64;

        case EPT_c128:
            return 128;
    }

    CPLAssert(false);
    return 1;
}

/************************************************************************/
/*                         HFAGetDataTypeName()                         */
/************************************************************************/

const char *HFAGetDataTypeName(EPTType eDataType)

{
    switch (eDataType)
    {
        case EPT_u1:
            return "u1";

        case EPT_u2:
            return "u2";

        case EPT_u4:
            return "u4";

        case EPT_u8:
            return "u8";

        case EPT_s8:
            return "s8";

        case EPT_u16:
            return "u16";

        case EPT_s16:
            return "s16";

        case EPT_u32:
            return "u32";

        case EPT_s32:
            return "s32";

        case EPT_f32:
            return "f32";

        case EPT_f64:
            return "f64";

        case EPT_c64:
            return "c64";

        case EPT_c128:
            return "c128";

        default:
            CPLAssert(false);
            return "unknown";
    }
}

/************************************************************************/
/*                           HFAGetMapInfo()                            */
/************************************************************************/

const Eprj_MapInfo *HFAGetMapInfo(HFAHandle hHFA)

{
    if (hHFA->nBands < 1)
        return nullptr;

    // Do we already have it?
    if (hHFA->pMapInfo != nullptr)
        return (Eprj_MapInfo *)hHFA->pMapInfo;

    // Get the HFA node.  If we don't find it under the usual name
    // we search for any node of the right type (#3338).
    HFAEntry *poMIEntry = hHFA->papoBand[0]->poNode->GetNamedChild("Map_Info");
    if (poMIEntry == nullptr)
    {
        for (HFAEntry *poChild = hHFA->papoBand[0]->poNode->GetChild();
             poChild != nullptr && poMIEntry == nullptr;
             poChild = poChild->GetNext())
        {
            if (EQUAL(poChild->GetType(), "Eprj_MapInfo"))
                poMIEntry = poChild;
        }
    }

    if (poMIEntry == nullptr)
    {
        return nullptr;
    }

    // Allocate the structure.
    Eprj_MapInfo *psMapInfo =
        static_cast<Eprj_MapInfo *>(CPLCalloc(sizeof(Eprj_MapInfo), 1));

    // Fetch the fields.
    psMapInfo->proName = CPLStrdup(poMIEntry->GetStringField("proName"));

    psMapInfo->upperLeftCenter.x =
        poMIEntry->GetDoubleField("upperLeftCenter.x");
    psMapInfo->upperLeftCenter.y =
        poMIEntry->GetDoubleField("upperLeftCenter.y");

    psMapInfo->lowerRightCenter.x =
        poMIEntry->GetDoubleField("lowerRightCenter.x");
    psMapInfo->lowerRightCenter.y =
        poMIEntry->GetDoubleField("lowerRightCenter.y");

    CPLErr eErr = CE_None;
    psMapInfo->pixelSize.width =
        poMIEntry->GetDoubleField("pixelSize.width", &eErr);
    psMapInfo->pixelSize.height =
        poMIEntry->GetDoubleField("pixelSize.height", &eErr);

    // The following is basically a hack to get files with
    // non-standard MapInfo's that misname the pixelSize fields. (#3338)
    if (eErr != CE_None)
    {
        psMapInfo->pixelSize.width = poMIEntry->GetDoubleField("pixelSize.x");
        psMapInfo->pixelSize.height = poMIEntry->GetDoubleField("pixelSize.y");
    }

    psMapInfo->units = CPLStrdup(poMIEntry->GetStringField("units"));

    hHFA->pMapInfo = (void *)psMapInfo;

    return psMapInfo;
}

/************************************************************************/
/*                        HFAInvGeoTransform()                          */
/************************************************************************/

static bool HFAInvGeoTransform(const double *gt_in, double *gt_out)

{
    // Assume a 3rd row that is [1 0 0].
    // Compute determinate.
    const double det = gt_in[1] * gt_in[5] - gt_in[2] * gt_in[4];

    if (fabs(det) < 1.0e-15)
        return false;

    const double inv_det = 1.0 / det;

    // Compute adjoint, and divide by determinate.
    gt_out[1] = gt_in[5] * inv_det;
    gt_out[4] = -gt_in[4] * inv_det;

    gt_out[2] = -gt_in[2] * inv_det;
    gt_out[5] = gt_in[1] * inv_det;

    gt_out[0] = (gt_in[2] * gt_in[3] - gt_in[0] * gt_in[5]) * inv_det;
    gt_out[3] = (-gt_in[1] * gt_in[3] + gt_in[0] * gt_in[4]) * inv_det;

    return true;
}

/************************************************************************/
/*                         HFAGetGeoTransform()                         */
/************************************************************************/

int HFAGetGeoTransform(HFAHandle hHFA, double *padfGeoTransform)

{
    const Eprj_MapInfo *psMapInfo = HFAGetMapInfo(hHFA);

    padfGeoTransform[0] = 0.0;
    padfGeoTransform[1] = 1.0;
    padfGeoTransform[2] = 0.0;
    padfGeoTransform[3] = 0.0;
    padfGeoTransform[4] = 0.0;
    padfGeoTransform[5] = 1.0;

    // Simple (north up) MapInfo approach.
    if (psMapInfo != nullptr)
    {
        padfGeoTransform[0] =
            psMapInfo->upperLeftCenter.x - psMapInfo->pixelSize.width * 0.5;
        padfGeoTransform[1] = psMapInfo->pixelSize.width;
        if (padfGeoTransform[1] == 0.0)
            padfGeoTransform[1] = 1.0;
        padfGeoTransform[2] = 0.0;
        if (psMapInfo->upperLeftCenter.y >= psMapInfo->lowerRightCenter.y)
            padfGeoTransform[5] = -psMapInfo->pixelSize.height;
        else
            padfGeoTransform[5] = psMapInfo->pixelSize.height;
        if (padfGeoTransform[5] == 0.0)
            padfGeoTransform[5] = 1.0;

        padfGeoTransform[3] =
            psMapInfo->upperLeftCenter.y - padfGeoTransform[5] * 0.5;
        padfGeoTransform[4] = 0.0;

        // Special logic to fixup odd angular units.
        if (EQUAL(psMapInfo->units, "ds"))
        {
            padfGeoTransform[0] /= 3600.0;
            padfGeoTransform[1] /= 3600.0;
            padfGeoTransform[2] /= 3600.0;
            padfGeoTransform[3] /= 3600.0;
            padfGeoTransform[4] /= 3600.0;
            padfGeoTransform[5] /= 3600.0;
        }

        return TRUE;
    }

    // Try for a MapToPixelXForm affine polynomial supporting
    // rotated and sheared affine transformations.
    if (hHFA->nBands == 0)
        return FALSE;

    HFAEntry *poXForm0 =
        hHFA->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm.XForm0");

    if (poXForm0 == nullptr)
        return FALSE;

    if (poXForm0->GetIntField("order") != 1 ||
        poXForm0->GetIntField("numdimtransform") != 2 ||
        poXForm0->GetIntField("numdimpolynomial") != 2 ||
        poXForm0->GetIntField("termcount") != 3)
        return FALSE;

    // Verify that there aren't any further xform steps.
    if (hHFA->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm.XForm1") !=
        nullptr)
        return FALSE;

    // We should check that the exponent list is 0 0 1 0 0 1, but
    // we don't because we are lazy.

    // Fetch geotransform values.
    double adfXForm[6] = {poXForm0->GetDoubleField("polycoefvector[0]"),
                          poXForm0->GetDoubleField("polycoefmtx[0]"),
                          poXForm0->GetDoubleField("polycoefmtx[2]"),
                          poXForm0->GetDoubleField("polycoefvector[1]"),
                          poXForm0->GetDoubleField("polycoefmtx[1]"),
                          poXForm0->GetDoubleField("polycoefmtx[3]")};

    // Invert.

    if (!HFAInvGeoTransform(adfXForm, padfGeoTransform))
        memset(padfGeoTransform, 0, 6 * sizeof(double));

    // Adjust origin from center of top left pixel to top left corner
    // of top left pixel.
    padfGeoTransform[0] -= padfGeoTransform[1] * 0.5;
    padfGeoTransform[0] -= padfGeoTransform[2] * 0.5;
    padfGeoTransform[3] -= padfGeoTransform[4] * 0.5;
    padfGeoTransform[3] -= padfGeoTransform[5] * 0.5;

    return TRUE;
}

/************************************************************************/
/*                           HFASetMapInfo()                            */
/************************************************************************/

CPLErr HFASetMapInfo(HFAHandle hHFA, const Eprj_MapInfo *poMapInfo)

{
    // Loop over bands, setting information on each one.
    for (int iBand = 0; iBand < hHFA->nBands; iBand++)
    {
        // Create a new Map_Info if there isn't one present already.
        HFAEntry *poMIEntry =
            hHFA->papoBand[iBand]->poNode->GetNamedChild("Map_Info");
        if (poMIEntry == nullptr)
        {
            poMIEntry = HFAEntry::New(hHFA, "Map_Info", "Eprj_MapInfo",
                                      hHFA->papoBand[iBand]->poNode);
        }

        poMIEntry->MarkDirty();

        // Ensure we have enough space for all the data.
        // TODO(schwehr): Explain 48 and 40 constants.
        const int nSize =
            static_cast<int>(48 + 40 + strlen(poMapInfo->proName) + 1 +
                             strlen(poMapInfo->units) + 1);

        GByte *pabyData = poMIEntry->MakeData(nSize);
        memset(pabyData, 0, nSize);

        poMIEntry->SetPosition();

        // Write the various fields.
        poMIEntry->SetStringField("proName", poMapInfo->proName);

        poMIEntry->SetDoubleField("upperLeftCenter.x",
                                  poMapInfo->upperLeftCenter.x);
        poMIEntry->SetDoubleField("upperLeftCenter.y",
                                  poMapInfo->upperLeftCenter.y);

        poMIEntry->SetDoubleField("lowerRightCenter.x",
                                  poMapInfo->lowerRightCenter.x);
        poMIEntry->SetDoubleField("lowerRightCenter.y",
                                  poMapInfo->lowerRightCenter.y);

        poMIEntry->SetDoubleField("pixelSize.width",
                                  poMapInfo->pixelSize.width);
        poMIEntry->SetDoubleField("pixelSize.height",
                                  poMapInfo->pixelSize.height);

        poMIEntry->SetStringField("units", poMapInfo->units);
    }

    return CE_None;
}

/************************************************************************/
/*                           HFAGetPEString()                           */
/*                                                                      */
/*      Some files have a ProjectionX node containing the ESRI style    */
/*      PE_STRING.  This function allows fetching from it.              */
/************************************************************************/

char *HFAGetPEString(HFAHandle hHFA)

{
    if (hHFA->nBands == 0)
        return nullptr;

    // Get the HFA node.
    HFAEntry *poProX = hHFA->papoBand[0]->poNode->GetNamedChild("ProjectionX");
    if (poProX == nullptr)
        return nullptr;

    const char *pszType = poProX->GetStringField("projection.type.string");
    if (pszType == nullptr || !EQUAL(pszType, "PE_COORDSYS"))
        return nullptr;

    // Use a gross hack to scan ahead to the actual projection
    // string. We do it this way because we don't have general
    // handling for MIFObjects.
    GByte *pabyData = poProX->GetData();
    int nDataSize = poProX->GetDataSize();

    while (nDataSize > 10 &&
           !STARTS_WITH_CI((const char *)pabyData, "PE_COORDSYS,."))
    {
        pabyData++;
        nDataSize--;
    }

    if (nDataSize < 31)
        return nullptr;

    // Skip ahead to the actual string.
    pabyData += 30;
    // nDataSize -= 30;

    return CPLStrdup((const char *)pabyData);
}

/************************************************************************/
/*                           HFASetPEString()                           */
/************************************************************************/

CPLErr HFASetPEString(HFAHandle hHFA, const char *pszPEString)

{
    if (!CPLTestBool(CPLGetConfigOption("HFA_WRITE_PE_STRING", "YES")))
        return CE_None;

    // Loop over bands, setting information on each one.
    for (int iBand = 0; iBand < hHFA->nBands; iBand++)
    {
        // Verify we don't already have the node, since update-in-place
        // is likely to be more complicated.
        HFAEntry *poProX =
            hHFA->papoBand[iBand]->poNode->GetNamedChild("ProjectionX");

        // If we are setting an empty string then a missing entry is equivalent.
        if (strlen(pszPEString) == 0 && poProX == nullptr)
            continue;

        // Create the node.
        if (poProX == nullptr)
        {
            poProX = HFAEntry::New(hHFA, "ProjectionX", "Eprj_MapProjection842",
                                   hHFA->papoBand[iBand]->poNode);
            if (poProX->GetTypeObject() == nullptr)
                return CE_Failure;
        }

        // Prepare the data area with some extra space just in case.
        GByte *pabyData =
            poProX->MakeData(static_cast<int>(700 + strlen(pszPEString)));
        if (!pabyData)
            return CE_Failure;

        memset(pabyData, 0, 250 + strlen(pszPEString));

        poProX->SetPosition();

        poProX->SetStringField("projection.type.string", "PE_COORDSYS");
        poProX->SetStringField("projection.MIFDictionary.string",
                               "{0:pcstring,}Emif_String,{1:x{0:pcstring,}"
                               "Emif_String,coordSys,}PE_COORDSYS,.");

        // Use a gross hack to scan ahead to the actual projection
        // string. We do it this way because we don't have general
        // handling for MIFObjects
        pabyData = poProX->GetData();
        int nDataSize = poProX->GetDataSize();
        GUInt32 iOffset = poProX->GetDataPos();

        while (nDataSize > 10 &&
               !STARTS_WITH_CI((const char *)pabyData, "PE_COORDSYS,."))
        {
            pabyData++;
            nDataSize--;
            iOffset++;
        }

        CPLAssert(nDataSize > static_cast<int>(strlen(pszPEString)) + 10);

        pabyData += 14;
        iOffset += 14;

        // Set the size and offset of the mifobject.
        iOffset += 8;

        GUInt32 nSize = static_cast<GUInt32>(strlen(pszPEString) + 9);

        HFAStandard(4, &nSize);
        memcpy(pabyData, &nSize, 4);
        pabyData += 4;

        HFAStandard(4, &iOffset);
        memcpy(pabyData, &iOffset, 4);
        pabyData += 4;

        // Set the size and offset of the string value.
        nSize = static_cast<GUInt32>(strlen(pszPEString) + 1);

        HFAStandard(4, &nSize);
        memcpy(pabyData, &nSize, 4);
        pabyData += 4;

        iOffset = 8;
        HFAStandard(4, &iOffset);
        memcpy(pabyData, &iOffset, 4);
        pabyData += 4;

        // Place the string itself.
        memcpy(pabyData, pszPEString, strlen(pszPEString) + 1);

        poProX->SetStringField("title.string", "PE");
    }

    return CE_None;
}

/************************************************************************/
/*                        HFAGetProParameters()                         */
/************************************************************************/

const Eprj_ProParameters *HFAGetProParameters(HFAHandle hHFA)

{
    if (hHFA->nBands < 1)
        return nullptr;

    // Do we already have it?
    if (hHFA->pProParameters != nullptr)
        return (Eprj_ProParameters *)hHFA->pProParameters;

    // Get the HFA node.
    HFAEntry *poMIEntry =
        hHFA->papoBand[0]->poNode->GetNamedChild("Projection");
    if (poMIEntry == nullptr)
        return nullptr;

    // Allocate the structure.
    Eprj_ProParameters *psProParams = static_cast<Eprj_ProParameters *>(
        CPLCalloc(sizeof(Eprj_ProParameters), 1));

    // Fetch the fields.
    const int proType = poMIEntry->GetIntField("proType");
    if (proType != EPRJ_INTERNAL && proType != EPRJ_EXTERNAL)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong value for proType");
        CPLFree(psProParams);
        return nullptr;
    }
    psProParams->proType = static_cast<Eprj_ProType>(proType);
    psProParams->proNumber = poMIEntry->GetIntField("proNumber");
    psProParams->proExeName =
        CPLStrdup(poMIEntry->GetStringField("proExeName"));
    psProParams->proName = CPLStrdup(poMIEntry->GetStringField("proName"));
    psProParams->proZone = poMIEntry->GetIntField("proZone");

    for (int i = 0; i < 15; i++)
    {
        char szFieldName[40] = {};

        snprintf(szFieldName, sizeof(szFieldName), "proParams[%d]", i);
        psProParams->proParams[i] = poMIEntry->GetDoubleField(szFieldName);
    }

    psProParams->proSpheroid.sphereName =
        CPLStrdup(poMIEntry->GetStringField("proSpheroid.sphereName"));
    psProParams->proSpheroid.a = poMIEntry->GetDoubleField("proSpheroid.a");
    psProParams->proSpheroid.b = poMIEntry->GetDoubleField("proSpheroid.b");
    psProParams->proSpheroid.eSquared =
        poMIEntry->GetDoubleField("proSpheroid.eSquared");
    psProParams->proSpheroid.radius =
        poMIEntry->GetDoubleField("proSpheroid.radius");

    hHFA->pProParameters = (void *)psProParams;

    return psProParams;
}

/************************************************************************/
/*                        HFASetProParameters()                         */
/************************************************************************/

CPLErr HFASetProParameters(HFAHandle hHFA, const Eprj_ProParameters *poPro)

{
    // Loop over bands, setting information on each one.
    for (int iBand = 0; iBand < hHFA->nBands; iBand++)
    {
        // Create a new Projection if there isn't one present already.
        HFAEntry *poMIEntry =
            hHFA->papoBand[iBand]->poNode->GetNamedChild("Projection");
        if (poMIEntry == nullptr)
        {
            poMIEntry = HFAEntry::New(hHFA, "Projection", "Eprj_ProParameters",
                                      hHFA->papoBand[iBand]->poNode);
        }

        poMIEntry->MarkDirty();

        // Ensure we have enough space for all the data.
        // TODO(schwehr): Explain all these constants.
        int nSize =
            static_cast<int>(34 + 15 * 8 + 8 + strlen(poPro->proName) + 1 + 32 +
                             8 + strlen(poPro->proSpheroid.sphereName) + 1);

        if (poPro->proExeName != nullptr)
            nSize += static_cast<int>(strlen(poPro->proExeName) + 1);

        GByte *pabyData = poMIEntry->MakeData(nSize);
        if (!pabyData)
            return CE_Failure;

        poMIEntry->SetPosition();

        // Initialize the whole thing to zeros for a clean start.
        memset(poMIEntry->GetData(), 0, poMIEntry->GetDataSize());

        // Write the various fields.
        poMIEntry->SetIntField("proType", poPro->proType);

        poMIEntry->SetIntField("proNumber", poPro->proNumber);

        poMIEntry->SetStringField("proExeName", poPro->proExeName);
        poMIEntry->SetStringField("proName", poPro->proName);
        poMIEntry->SetIntField("proZone", poPro->proZone);
        poMIEntry->SetDoubleField("proParams[0]", poPro->proParams[0]);
        poMIEntry->SetDoubleField("proParams[1]", poPro->proParams[1]);
        poMIEntry->SetDoubleField("proParams[2]", poPro->proParams[2]);
        poMIEntry->SetDoubleField("proParams[3]", poPro->proParams[3]);
        poMIEntry->SetDoubleField("proParams[4]", poPro->proParams[4]);
        poMIEntry->SetDoubleField("proParams[5]", poPro->proParams[5]);
        poMIEntry->SetDoubleField("proParams[6]", poPro->proParams[6]);
        poMIEntry->SetDoubleField("proParams[7]", poPro->proParams[7]);
        poMIEntry->SetDoubleField("proParams[8]", poPro->proParams[8]);
        poMIEntry->SetDoubleField("proParams[9]", poPro->proParams[9]);
        poMIEntry->SetDoubleField("proParams[10]", poPro->proParams[10]);
        poMIEntry->SetDoubleField("proParams[11]", poPro->proParams[11]);
        poMIEntry->SetDoubleField("proParams[12]", poPro->proParams[12]);
        poMIEntry->SetDoubleField("proParams[13]", poPro->proParams[13]);
        poMIEntry->SetDoubleField("proParams[14]", poPro->proParams[14]);
        poMIEntry->SetStringField("proSpheroid.sphereName",
                                  poPro->proSpheroid.sphereName);
        poMIEntry->SetDoubleField("proSpheroid.a", poPro->proSpheroid.a);
        poMIEntry->SetDoubleField("proSpheroid.b", poPro->proSpheroid.b);
        poMIEntry->SetDoubleField("proSpheroid.eSquared",
                                  poPro->proSpheroid.eSquared);
        poMIEntry->SetDoubleField("proSpheroid.radius",
                                  poPro->proSpheroid.radius);
    }

    return CE_None;
}

/************************************************************************/
/*                            HFAGetDatum()                             */
/************************************************************************/

const Eprj_Datum *HFAGetDatum(HFAHandle hHFA)

{
    if (hHFA->nBands < 1)
        return nullptr;

    // Do we already have it?
    if (hHFA->pDatum != nullptr)
        return (Eprj_Datum *)hHFA->pDatum;

    // Get the HFA node.
    HFAEntry *poMIEntry =
        hHFA->papoBand[0]->poNode->GetNamedChild("Projection.Datum");
    if (poMIEntry == nullptr)
        return nullptr;

    // Allocate the structure.
    Eprj_Datum *psDatum =
        static_cast<Eprj_Datum *>(CPLCalloc(sizeof(Eprj_Datum), 1));

    // Fetch the fields.
    psDatum->datumname = CPLStrdup(poMIEntry->GetStringField("datumname"));
    const int nDatumType = poMIEntry->GetIntField("type");
    if (nDatumType < 0 || nDatumType > EPRJ_DATUM_NONE)
    {
        CPLDebug("HFA", "Invalid value for datum type: %d", nDatumType);
        psDatum->type = EPRJ_DATUM_NONE;
    }
    else
        psDatum->type = static_cast<Eprj_DatumType>(nDatumType);

    for (int i = 0; i < 7; i++)
    {
        char szFieldName[30] = {};
        snprintf(szFieldName, sizeof(szFieldName), "params[%d]", i);
        psDatum->params[i] = poMIEntry->GetDoubleField(szFieldName);
    }

    psDatum->gridname = CPLStrdup(poMIEntry->GetStringField("gridname"));

    hHFA->pDatum = (void *)psDatum;

    return psDatum;
}

/************************************************************************/
/*                            HFASetDatum()                             */
/************************************************************************/

CPLErr HFASetDatum(HFAHandle hHFA, const Eprj_Datum *poDatum)

{
    // Loop over bands, setting information on each one.
    for (int iBand = 0; iBand < hHFA->nBands; iBand++)
    {
        // Create a new Projection if there isn't one present already.
        HFAEntry *poProParams =
            hHFA->papoBand[iBand]->poNode->GetNamedChild("Projection");
        if (poProParams == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Can't add Eprj_Datum with no Eprj_ProjParameters.");
            return CE_Failure;
        }

        HFAEntry *poDatumEntry = poProParams->GetNamedChild("Datum");
        if (poDatumEntry == nullptr)
        {
            poDatumEntry =
                HFAEntry::New(hHFA, "Datum", "Eprj_Datum", poProParams);
        }

        poDatumEntry->MarkDirty();

        // Ensure we have enough space for all the data.
        // TODO(schwehr): Explain constants.
        int nSize =
            static_cast<int>(26 + strlen(poDatum->datumname) + 1 + 7 * 8);

        if (poDatum->gridname != nullptr)
            nSize += static_cast<int>(strlen(poDatum->gridname) + 1);

        GByte *pabyData = poDatumEntry->MakeData(nSize);
        if (!pabyData)
            return CE_Failure;

        poDatumEntry->SetPosition();

        // Initialize the whole thing to zeros for a clean start.
        memset(poDatumEntry->GetData(), 0, poDatumEntry->GetDataSize());

        // Write the various fields.
        poDatumEntry->SetStringField("datumname", poDatum->datumname);
        poDatumEntry->SetIntField("type", poDatum->type);

        poDatumEntry->SetDoubleField("params[0]", poDatum->params[0]);
        poDatumEntry->SetDoubleField("params[1]", poDatum->params[1]);
        poDatumEntry->SetDoubleField("params[2]", poDatum->params[2]);
        poDatumEntry->SetDoubleField("params[3]", poDatum->params[3]);
        poDatumEntry->SetDoubleField("params[4]", poDatum->params[4]);
        poDatumEntry->SetDoubleField("params[5]", poDatum->params[5]);
        poDatumEntry->SetDoubleField("params[6]", poDatum->params[6]);

        poDatumEntry->SetStringField("gridname", poDatum->gridname);
    }

    return CE_None;
}

/************************************************************************/
/*                             HFAGetPCT()                              */
/*                                                                      */
/*      Read the PCT from a band, if it has one.                        */
/************************************************************************/

CPLErr HFAGetPCT(HFAHandle hHFA, int nBand, int *pnColors, double **ppadfRed,
                 double **ppadfGreen, double **ppadfBlue, double **ppadfAlpha,
                 double **ppadfBins)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->GetPCT(pnColors, ppadfRed, ppadfGreen,
                                             ppadfBlue, ppadfAlpha, ppadfBins);
}

/************************************************************************/
/*                             HFASetPCT()                              */
/*                                                                      */
/*      Set the PCT on a band.                                          */
/************************************************************************/

CPLErr HFASetPCT(HFAHandle hHFA, int nBand, int nColors, double *padfRed,
                 double *padfGreen, double *padfBlue, double *padfAlpha)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    return hHFA->papoBand[nBand - 1]->SetPCT(nColors, padfRed, padfGreen,
                                             padfBlue, padfAlpha);
}

/************************************************************************/
/*                          HFAGetDataRange()                           */
/************************************************************************/

CPLErr HFAGetDataRange(HFAHandle hHFA, int nBand, double *pdfMin,
                       double *pdfMax)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return CE_Failure;

    HFAEntry *poBinInfo =
        hHFA->papoBand[nBand - 1]->poNode->GetNamedChild("Statistics");

    if (poBinInfo == nullptr)
        return CE_Failure;

    *pdfMin = poBinInfo->GetDoubleField("minimum");
    *pdfMax = poBinInfo->GetDoubleField("maximum");

    if (*pdfMax > *pdfMin)
        return CE_None;
    else
        return CE_Failure;
}

/************************************************************************/
/*                            HFADumpNode()                             */
/************************************************************************/

static void HFADumpNode(HFAEntry *poEntry, int nIndent, bool bVerbose, FILE *fp)

{
    std::string osSpaces(nIndent * 2, ' ');

    fprintf(fp, "%s%s(%s) @ %u + %u @ %u\n", osSpaces.c_str(),
            poEntry->GetName(), poEntry->GetType(), poEntry->GetFilePos(),
            poEntry->GetDataSize(), poEntry->GetDataPos());

    if (bVerbose)
    {
        osSpaces += "+ ";
        poEntry->DumpFieldValues(fp, osSpaces.c_str());
        fprintf(fp, "\n");
    }

    if (poEntry->GetChild() != nullptr)
        HFADumpNode(poEntry->GetChild(), nIndent + 1, bVerbose, fp);

    if (poEntry->GetNext() != nullptr)
        HFADumpNode(poEntry->GetNext(), nIndent, bVerbose, fp);
}

/************************************************************************/
/*                            HFADumpTree()                             */
/*                                                                      */
/*      Dump the tree of information in a HFA file.                     */
/************************************************************************/

void HFADumpTree(HFAHandle hHFA, FILE *fpOut)

{
    HFADumpNode(hHFA->poRoot, 0, true, fpOut);
}

/************************************************************************/
/*                         HFADumpDictionary()                          */
/*                                                                      */
/*      Dump the dictionary (in raw, and parsed form) to the named      */
/*      device.                                                         */
/************************************************************************/

void HFADumpDictionary(HFAHandle hHFA, FILE *fpOut)

{
    fprintf(fpOut, "%s\n", hHFA->pszDictionary);

    hHFA->poDictionary->Dump(fpOut);
}

/************************************************************************/
/*                            HFAStandard()                             */
/*                                                                      */
/*      Swap byte order on MSB systems.                                 */
/************************************************************************/

#ifdef CPL_MSB
void HFAStandard(int nBytes, void *pData)

{
    GByte *pabyData = static_cast<GByte *>(pData);

    for (int i = nBytes / 2 - 1; i >= 0; i--)
    {
        GByte byTemp = pabyData[i];
        pabyData[i] = pabyData[nBytes - i - 1];
        pabyData[nBytes - i - 1] = byTemp;
    }
}
#endif

/* ==================================================================== */
/*      Default data dictionary.  Emitted verbatim into the imagine     */
/*      file.                                                           */
/* ==================================================================== */

static const char *const aszDefaultDD[] = {
    "{1:lversion,1:LfreeList,1:LrootEntryPtr,1:sentryHeaderLength,1:"
    "LdictionaryPtr,}Ehfa_File,{1:Lnext,1:Lprev,1:Lparent,1:Lchild,1:Ldata,1:"
    "ldataSize,64:cname,32:ctype,1:tmodTime,}Ehfa_Entry,{16:clabel,1:"
    "LheaderPtr,}Ehfa_HeaderTag,{1:LfreeList,1:lfreeSize,}Ehfa_FreeListNode,{1:"
    "lsize,1:Lptr,}Ehfa_Data,{1:lwidth,1:lheight,1:e3:thematic,athematic,fft "
    "of real-valued data,layerType,",
    "1:e13:u1,u2,u4,u8,s8,u16,s16,u32,s32,f32,f64,c64,c128,pixelType,1:"
    "lblockWidth,1:lblockHeight,}Eimg_Layer,{1:lwidth,1:lheight,1:e3:thematic,"
    "athematic,fft of real-valued "
    "data,layerType,1:e13:u1,u2,u4,u8,s8,u16,s16,u32,s32,f32,f64,c64,c128,"
    "pixelType,1:lblockWidth,1:lblockHeight,}Eimg_Layer_SubSample,{1:e2:raster,"
    "vector,type,1:LdictionaryPtr,}Ehfa_Layer,{1:LspaceUsedForRasterData,}"
    "ImgFormatInfo831,{1:sfileCode,1:Loffset,1:lsize,1:e2:false,true,logvalid,",
    "1:e2:no compression,ESRI GRID "
    "compression,compressionType,}Edms_VirtualBlockInfo,{1:lmin,1:lmax,}Edms_"
    "FreeIDList,{1:lnumvirtualblocks,1:lnumobjectsperblock,1:lnextobjectnum,1:"
    "e2:no compression,RLC "
    "compression,compressionType,0:poEdms_VirtualBlockInfo,blockinfo,0:poEdms_"
    "FreeIDList,freelist,1:tmodTime,}Edms_State,{0:pcstring,}Emif_String,{1:"
    "oEmif_String,fileName,2:LlayerStackValidFlagsOffset,2:"
    "LlayerStackDataOffset,1:LlayerStackCount,1:LlayerStackIndex,}"
    "ImgExternalRaster,{1:oEmif_String,algorithm,0:poEmif_String,nameList,}"
    "Eimg_RRDNamesList,{1:oEmif_String,projection,1:oEmif_String,units,}Eimg_"
    "MapInformation,",
    "{1:oEmif_String,dependent,}Eimg_DependentFile,{1:oEmif_String,"
    "ImageLayerName,}Eimg_DependentLayerName,{1:lnumrows,1:lnumcolumns,1:e13:"
    "EGDA_TYPE_U1,EGDA_TYPE_U2,EGDA_TYPE_U4,EGDA_TYPE_U8,EGDA_TYPE_S8,EGDA_"
    "TYPE_U16,EGDA_TYPE_S16,EGDA_TYPE_U32,EGDA_TYPE_S32,EGDA_TYPE_F32,EGDA_"
    "TYPE_F64,EGDA_TYPE_C64,EGDA_TYPE_C128,datatype,1:e4:EGDA_SCALAR_OBJECT,"
    "EGDA_TABLE_OBJECT,EGDA_MATRIX_OBJECT,EGDA_RASTER_OBJECT,objecttype,}Egda_"
    "BaseData,{1:*bvalueBD,}Eimg_NonInitializedValue,{1:dx,1:dy,}Eprj_"
    "Coordinate,{1:dwidth,1:dheight,}Eprj_Size,{0:pcproName,1:*oEprj_"
    "Coordinate,upperLeftCenter,",
    "1:*oEprj_Coordinate,lowerRightCenter,1:*oEprj_Size,pixelSize,0:pcunits,}"
    "Eprj_MapInfo,{0:pcdatumname,1:e3:EPRJ_DATUM_PARAMETRIC,EPRJ_DATUM_GRID,"
    "EPRJ_DATUM_REGRESSION,type,0:pdparams,0:pcgridname,}Eprj_Datum,{0:"
    "pcsphereName,1:da,1:db,1:deSquared,1:dradius,}Eprj_Spheroid,{1:e2:EPRJ_"
    "INTERNAL,EPRJ_EXTERNAL,proType,1:lproNumber,0:pcproExeName,0:pcproName,1:"
    "lproZone,0:pdproParams,1:*oEprj_Spheroid,proSpheroid,}Eprj_ProParameters,{"
    "1:dminimum,1:dmaximum,1:dmean,1:dmedian,1:dmode,1:dstddev,}Esta_"
    "Statistics,{1:lnumBins,1:e4:direct,linear,logarithmic,explicit,"
    "binFunctionType,1:dminLimit,1:dmaxLimit,1:*bbinLimits,}Edsc_BinFunction,{"
    "0:poEmif_String,LayerNames,1:*bExcludedValues,1:oEmif_String,AOIname,",
    "1:lSkipFactorX,1:lSkipFactorY,1:*oEdsc_BinFunction,BinFunction,}Eimg_"
    "StatisticsParameters830,{1:lnumrows,}Edsc_Table,{1:lnumRows,1:"
    "LcolumnDataPtr,1:e4:integer,real,complex,string,dataType,1:lmaxNumChars,}"
    "Edsc_Column,{1:lposition,0:pcname,1:e2:EMSC_FALSE,EMSC_TRUE,editable,1:e3:"
    "LEFT,CENTER,RIGHT,alignment,0:pcformat,1:e3:DEFAULT,APPLY,AUTO-APPLY,"
    "formulamode,0:pcformula,1:dcolumnwidth,0:pcunits,1:e5:NO_COLOR,RED,GREEN,"
    "BLUE,COLOR,colorflag,0:pcgreenname,0:pcbluename,}Eded_ColumnAttributes_1,{"
    "1:lversion,1:lnumobjects,1:e2:EAOI_UNION,EAOI_INTERSECTION,operation,}"
    "Eaoi_AreaOfInterest,",
    "{1:x{0:pcstring,}Emif_String,type,1:x{0:pcstring,}Emif_String,"
    "MIFDictionary,0:pCMIFObject,}Emif_MIFObject,",
    "{1:x{1:x{0:pcstring,}Emif_String,type,1:x{0:pcstring,}Emif_String,"
    "MIFDictionary,0:pCMIFObject,}Emif_MIFObject,projection,1:x{0:pcstring,}"
    "Emif_String,title,}Eprj_MapProjection842,",
    "{0:poEmif_String,titleList,}Exfr_GenericXFormHeader,{1:lorder,1:"
    "lnumdimtransform,1:lnumdimpolynomial,1:ltermcount,0:plexponentlist,1:*"
    "bpolycoefmtx,1:*bpolycoefvector,}Efga_Polynomial,",
    ".",
    nullptr};

/************************************************************************/
/*                            HFACreateLL()                             */
/*                                                                      */
/*      Low level creation of an Imagine file.  Writes out the          */
/*      Ehfa_HeaderTag, dictionary and Ehfa_File.                       */
/************************************************************************/

HFAHandle HFACreateLL(const char *pszFilename)

{
    // Create the file in the file system.
    VSILFILE *fp = VSIFOpenL(pszFilename, "w+b");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Creation of file %s failed.",
                 pszFilename);
        return nullptr;
    }

    // Create the HFAInfo_t.
    HFAInfo_t *psInfo =
        static_cast<HFAInfo_t *>(CPLCalloc(sizeof(HFAInfo_t), 1));

    psInfo->fp = fp;
    psInfo->eAccess = HFA_Update;
    psInfo->nXSize = 0;
    psInfo->nYSize = 0;
    psInfo->nBands = 0;
    psInfo->papoBand = nullptr;
    psInfo->pMapInfo = nullptr;
    psInfo->pDatum = nullptr;
    psInfo->pProParameters = nullptr;
    psInfo->bTreeDirty = false;
    psInfo->pszFilename = CPLStrdup(CPLGetFilename(pszFilename));
    psInfo->pszPath = CPLStrdup(CPLGetPathSafe(pszFilename).c_str());

    // Write out the Ehfa_HeaderTag.
    bool bRet = VSIFWriteL((void *)"EHFA_HEADER_TAG", 1, 16, fp) > 0;

    GInt32 nHeaderPos = 20;
    HFAStandard(4, &nHeaderPos);
    bRet &= VSIFWriteL(&nHeaderPos, 4, 1, fp) > 0;

    // Write the Ehfa_File node, locked in at offset 20.
    GInt32 nVersion = 1;
    GInt32 nFreeList = 0;
    GInt32 nRootEntry = 0;
    GInt16 nEntryHeaderLength = 128;
    GInt32 nDictionaryPtr = 38;

    psInfo->nEntryHeaderLength = nEntryHeaderLength;
    psInfo->nRootPos = 0;
    psInfo->nDictionaryPos = nDictionaryPtr;
    psInfo->nVersion = nVersion;

    HFAStandard(4, &nVersion);
    HFAStandard(4, &nFreeList);
    HFAStandard(4, &nRootEntry);
    HFAStandard(2, &nEntryHeaderLength);
    HFAStandard(4, &nDictionaryPtr);

    bRet &= VSIFWriteL(&nVersion, 4, 1, fp) > 0;
    bRet &= VSIFWriteL(&nFreeList, 4, 1, fp) > 0;
    bRet &= VSIFWriteL(&nRootEntry, 4, 1, fp) > 0;
    bRet &= VSIFWriteL(&nEntryHeaderLength, 2, 1, fp) > 0;
    bRet &= VSIFWriteL(&nDictionaryPtr, 4, 1, fp) > 0;

    // Write the dictionary, locked in at location 38.  Note that
    // we jump through a bunch of hoops to operate on the
    // dictionary in chunks because some compiles (such as VC++)
    // don't allow particularly large static strings.
    int nDictLen = 0;

    for (int iChunk = 0; aszDefaultDD[iChunk] != nullptr; iChunk++)
        nDictLen += static_cast<int>(strlen(aszDefaultDD[iChunk]));

    psInfo->pszDictionary = static_cast<char *>(CPLMalloc(nDictLen + 1));
    psInfo->pszDictionary[0] = '\0';

    for (int iChunk = 0; aszDefaultDD[iChunk] != nullptr; iChunk++)
        strcat(psInfo->pszDictionary, aszDefaultDD[iChunk]);

    bRet &= VSIFWriteL((void *)psInfo->pszDictionary,
                       strlen(psInfo->pszDictionary) + 1, 1, fp) > 0;
    if (!bRet)
    {
        CPL_IGNORE_RET_VAL(HFAClose(psInfo));
        return nullptr;
    }

    psInfo->poDictionary = new HFADictionary(psInfo->pszDictionary);

    psInfo->nEndOfFile = static_cast<GUInt32>(VSIFTellL(fp));

    // Create a root entry.
    psInfo->poRoot = new HFAEntry(psInfo, "root", "root", nullptr);

    // If an .ige or .rrd file exists with the same base name,
    // delete them.  (#1784)
    CPLString osExtension = CPLGetExtensionSafe(pszFilename);
    if (!EQUAL(osExtension, "rrd") && !EQUAL(osExtension, "aux"))
    {
        CPLString osPath = CPLGetPathSafe(pszFilename);
        CPLString osBasename = CPLGetBasenameSafe(pszFilename);
        VSIStatBufL sStatBuf;
        CPLString osSupFile = CPLFormCIFilenameSafe(osPath, osBasename, "rrd");

        if (VSIStatL(osSupFile, &sStatBuf) == 0)
            VSIUnlink(osSupFile);

        osSupFile = CPLFormCIFilenameSafe(osPath, osBasename, "ige");

        if (VSIStatL(osSupFile, &sStatBuf) == 0)
            VSIUnlink(osSupFile);
    }

    return psInfo;
}

/************************************************************************/
/*                          HFAAllocateSpace()                          */
/*                                                                      */
/*      Return an area in the file to the caller to write the           */
/*      requested number of bytes.  Currently this is always at the     */
/*      end of the file, but eventually we might actually keep track    */
/*      of free space.  The HFAInfo_t's concept of file size is         */
/*      updated, even if nothing ever gets written to this region.      */
/*                                                                      */
/*      Returns the offset to the requested space, or zero one          */
/*      failure.                                                        */
/************************************************************************/

GUInt32 HFAAllocateSpace(HFAInfo_t *psInfo, GUInt32 nBytes)

{
    // TODO(schwehr): Check if this will wrap over 2GB limit.

    psInfo->nEndOfFile += nBytes;
    return psInfo->nEndOfFile - nBytes;
}

/************************************************************************/
/*                              HFAFlush()                              */
/*                                                                      */
/*      Write out any dirty tree information to disk, putting the       */
/*      disk file in a consistent state.                                */
/************************************************************************/

CPLErr HFAFlush(HFAHandle hHFA)

{
    if (!hHFA->bTreeDirty && !hHFA->poDictionary->bDictionaryTextDirty)
        return CE_None;

    CPLAssert(hHFA->poRoot != nullptr);

    // Flush HFAEntry tree to disk.
    if (hHFA->bTreeDirty)
    {
        const CPLErr eErr = hHFA->poRoot->FlushToDisk();
        if (eErr != CE_None)
            return eErr;

        hHFA->bTreeDirty = false;
    }

    // Flush Dictionary to disk.
    GUInt32 nNewDictionaryPos = hHFA->nDictionaryPos;
    bool bRet = true;
    if (hHFA->poDictionary->bDictionaryTextDirty)
    {
        bRet &= VSIFSeekL(hHFA->fp, 0, SEEK_END) >= 0;
        nNewDictionaryPos = static_cast<GUInt32>(VSIFTellL(hHFA->fp));
        bRet &=
            VSIFWriteL(hHFA->poDictionary->osDictionaryText.c_str(),
                       strlen(hHFA->poDictionary->osDictionaryText.c_str()) + 1,
                       1, hHFA->fp) > 0;
        hHFA->poDictionary->bDictionaryTextDirty = false;
    }

    // Do we need to update the Ehfa_File pointer to the root node?
    if (hHFA->nRootPos != hHFA->poRoot->GetFilePos() ||
        nNewDictionaryPos != hHFA->nDictionaryPos)
    {
        GUInt32 nHeaderPos = 0;

        bRet &= VSIFSeekL(hHFA->fp, 16, SEEK_SET) >= 0;
        bRet &= VSIFReadL(&nHeaderPos, sizeof(GInt32), 1, hHFA->fp) > 0;
        HFAStandard(4, &nHeaderPos);

        GUInt32 nOffset = hHFA->poRoot->GetFilePos();
        hHFA->nRootPos = nOffset;
        HFAStandard(4, &nOffset);
        bRet &= VSIFSeekL(hHFA->fp, nHeaderPos + 8, SEEK_SET) >= 0;
        bRet &= VSIFWriteL(&nOffset, 4, 1, hHFA->fp) > 0;

        nOffset = nNewDictionaryPos;
        hHFA->nDictionaryPos = nNewDictionaryPos;
        HFAStandard(4, &nOffset);
        bRet &= VSIFSeekL(hHFA->fp, nHeaderPos + 14, SEEK_SET) >= 0;
        bRet &= VSIFWriteL(&nOffset, 4, 1, hHFA->fp) > 0;
    }

    return bRet ? CE_None : CE_Failure;
}

/************************************************************************/
/*                           HFACreateLayer()                           */
/*                                                                      */
/*      Create a layer object, and corresponding RasterDMS.             */
/*      Suitable for use with primary layers, and overviews.            */
/************************************************************************/

int HFACreateLayer(HFAHandle psInfo, HFAEntry *poParent,
                   const char *pszLayerName, int bOverview, int nBlockSize,
                   int bCreateCompressed, int bCreateLargeRaster,
                   int bDependentLayer, int nXSize, int nYSize,
                   EPTType eDataType, char ** /* papszOptions */,
                   // These are only related to external (large) files.
                   GIntBig nStackValidFlagsOffset, GIntBig nStackDataOffset,
                   int nStackCount, int nStackIndex)

{
    const char *pszLayerType =
        bOverview ? "Eimg_Layer_SubSample" : "Eimg_Layer";

    if (nBlockSize <= 0)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "HFACreateLayer: nBlockXSize < 0");
        return FALSE;
    }

    // Work out some details about the tiling scheme.
    const int nBlocksPerRow = DIV_ROUND_UP(nXSize, nBlockSize);
    const int nBlocksPerColumn = DIV_ROUND_UP(nYSize, nBlockSize);
    const int nBlocks = nBlocksPerRow * nBlocksPerColumn;
    const int nBytesPerBlock =
        (nBlockSize * nBlockSize * HFAGetDataTypeBits(eDataType) + 7) / 8;

    // Create the Eimg_Layer for the band.
    HFAEntry *poEimg_Layer =
        HFAEntry::New(psInfo, pszLayerName, pszLayerType, poParent);

    poEimg_Layer->SetIntField("width", nXSize);
    poEimg_Layer->SetIntField("height", nYSize);
    poEimg_Layer->SetStringField("layerType", "athematic");
    poEimg_Layer->SetIntField("pixelType", eDataType);
    poEimg_Layer->SetIntField("blockWidth", nBlockSize);
    poEimg_Layer->SetIntField("blockHeight", nBlockSize);

    // Create the RasterDMS (block list).  This is a complex type
    // with pointers, and variable size.  We set the superstructure
    // ourselves rather than trying to have the HFA type management
    // system do it for us (since this would be hard to implement).
    if (!bCreateLargeRaster && !bDependentLayer)
    {
        HFAEntry *poEdms_State =
            HFAEntry::New(psInfo, "RasterDMS", "Edms_State", poEimg_Layer);

        // TODO(schwehr): Explain constants.
        const int nDmsSize = 14 * nBlocks + 38;
        GByte *pabyData = poEdms_State->MakeData(nDmsSize);

        // Set some simple values.
        poEdms_State->SetIntField("numvirtualblocks", nBlocks);
        poEdms_State->SetIntField("numobjectsperblock",
                                  nBlockSize * nBlockSize);
        poEdms_State->SetIntField("nextobjectnum",
                                  nBlockSize * nBlockSize * nBlocks);

        // Is file compressed or not?
        if (bCreateCompressed)
        {
            poEdms_State->SetStringField("compressionType", "RLC compression");
        }
        else
        {
            poEdms_State->SetStringField("compressionType", "no compression");
        }

        // We need to hardcode file offset into the data, so locate it now.
        poEdms_State->SetPosition();

        // Set block info headers.

        // Blockinfo count.
        GUInt32 nValue = nBlocks;
        HFAStandard(4, &nValue);
        memcpy(pabyData + 14, &nValue, 4);

        // Blockinfo position.
        nValue = poEdms_State->GetDataPos() + 22;
        HFAStandard(4, &nValue);
        memcpy(pabyData + 18, &nValue, 4);

        // Set each blockinfo.
        for (int iBlock = 0; iBlock < nBlocks; iBlock++)
        {
            int nOffset = 22 + 14 * iBlock;

            // fileCode.
            GInt16 nValue16 = 0;
            HFAStandard(2, &nValue16);
            memcpy(pabyData + nOffset, &nValue16, 2);

            // Offset.
            if (bCreateCompressed)
            {
                // Flag it with zero offset. Allocate space when we compress it.
                nValue = 0;
            }
            else
            {
                nValue = HFAAllocateSpace(psInfo, nBytesPerBlock);
            }
            HFAStandard(4, &nValue);
            memcpy(pabyData + nOffset + 2, &nValue, 4);

            // Size.
            if (bCreateCompressed)
            {
                // Flag with zero size. Don't know until we compress it.
                nValue = 0;
            }
            else
            {
                nValue = nBytesPerBlock;
            }
            HFAStandard(4, &nValue);
            memcpy(pabyData + nOffset + 6, &nValue, 4);

            // logValid (false).
            nValue16 = 0;
            HFAStandard(2, &nValue16);
            memcpy(pabyData + nOffset + 10, &nValue16, 2);

            // compressionType.
            if (bCreateCompressed)
                nValue16 = 1;
            else
                nValue16 = 0;

            HFAStandard(2, &nValue16);
            memcpy(pabyData + nOffset + 12, &nValue16, 2);
        }
    }

    // Create ExternalRasterDMS object.
    else if (bCreateLargeRaster)
    {
        HFAEntry *poEdms_State = HFAEntry::New(
            psInfo, "ExternalRasterDMS", "ImgExternalRaster", poEimg_Layer);
        poEdms_State->MakeData(
            static_cast<int>(8 + strlen(psInfo->pszIGEFilename) + 1 + 6 * 4));

        poEdms_State->SetStringField("fileName.string", psInfo->pszIGEFilename);

        poEdms_State->SetIntField(
            "layerStackValidFlagsOffset[0]",
            static_cast<int>(nStackValidFlagsOffset & 0xFFFFFFFF));
        poEdms_State->SetIntField(
            "layerStackValidFlagsOffset[1]",
            static_cast<int>(nStackValidFlagsOffset >> 32));

        poEdms_State->SetIntField(
            "layerStackDataOffset[0]",
            static_cast<int>(nStackDataOffset & 0xFFFFFFFF));
        poEdms_State->SetIntField("layerStackDataOffset[1]",
                                  static_cast<int>(nStackDataOffset >> 32));
        poEdms_State->SetIntField("layerStackCount", nStackCount);
        poEdms_State->SetIntField("layerStackIndex", nStackIndex);
    }
    // Dependent...
    else if (bDependentLayer)
    {
        HFAEntry *poDepLayerName =
            HFAEntry::New(psInfo, "DependentLayerName",
                          "Eimg_DependentLayerName", poEimg_Layer);
        poDepLayerName->MakeData(
            static_cast<int>(8 + strlen(pszLayerName) + 2));

        poDepLayerName->SetStringField("ImageLayerName.string", pszLayerName);
    }

    // Create the Ehfa_Layer.
    char chBandType = '\0';

    if (eDataType == EPT_u1)
        chBandType = '1';
    else if (eDataType == EPT_u2)
        chBandType = '2';
    else if (eDataType == EPT_u4)
        chBandType = '4';
    else if (eDataType == EPT_u8)
        chBandType = 'c';
    else if (eDataType == EPT_s8)
        chBandType = 'C';
    else if (eDataType == EPT_u16)
        chBandType = 's';
    else if (eDataType == EPT_s16)
        chBandType = 'S';
    else if (eDataType == EPT_u32)
        // For some reason erdas imagine expects an L for unsigned 32 bit ints
        // otherwise it gives strange "out of memory errors".
        chBandType = 'L';
    else if (eDataType == EPT_s32)
        chBandType = 'L';
    else if (eDataType == EPT_f32)
        chBandType = 'f';
    else if (eDataType == EPT_f64)
        chBandType = 'd';
    else if (eDataType == EPT_c64)
        chBandType = 'm';
    else if (eDataType == EPT_c128)
        chBandType = 'M';
    else
    {
        CPLAssert(false);
        chBandType = 'c';
    }

    // The first value in the entry below gives the number of pixels
    // within a block.
    char szLDict[128] = {};
    snprintf(szLDict, sizeof(szLDict), "{%d:%cdata,}RasterDMS,.",
             nBlockSize * nBlockSize, chBandType);

    HFAEntry *poEhfa_Layer =
        HFAEntry::New(psInfo, "Ehfa_Layer", "Ehfa_Layer", poEimg_Layer);
    poEhfa_Layer->MakeData();
    poEhfa_Layer->SetPosition();
    const GUInt32 nLDict =
        HFAAllocateSpace(psInfo, static_cast<GUInt32>(strlen(szLDict) + 1));

    poEhfa_Layer->SetStringField("type", "raster");
    poEhfa_Layer->SetIntField("dictionaryPtr", nLDict);

    bool bRet = VSIFSeekL(psInfo->fp, nLDict, SEEK_SET) >= 0;
    bRet &= VSIFWriteL((void *)szLDict, strlen(szLDict) + 1, 1, psInfo->fp) > 0;

    return bRet;
}

/************************************************************************/
/*                             HFACreate()                              */
/************************************************************************/

HFAHandle HFACreate(const char *pszFilename, int nXSize, int nYSize, int nBands,
                    EPTType eDataType, char **papszOptions)

{
    if (nXSize == 0 || nYSize == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "nXSize == 0 || nYSize == 0 not supported");
        return nullptr;
    }
    int nBlockSize = 64;
    const char *pszValue = CSLFetchNameValue(papszOptions, "BLOCKSIZE");

    if (pszValue != nullptr)
    {
        nBlockSize = atoi(pszValue);
        // Check for sane values.
        if (nBlockSize == 0 ||
            (((nBlockSize < 32) || (nBlockSize > 2048)) &&
             !CPLTestBool(CPLGetConfigOption("FORCE_BLOCKSIZE", "NO"))))
        {
            if (nBlockSize != 0)
                CPLError(CE_Warning, CPLE_AppDefined, "Forcing BLOCKSIZE to %d",
                         64);
            nBlockSize = 64;
        }
    }
    bool bCreateLargeRaster = CPLFetchBool(papszOptions, "USE_SPILL", false);
    bool bCreateCompressed = CPLFetchBool(papszOptions, "COMPRESS", false) ||
                             CPLFetchBool(papszOptions, "COMPRESSED", false);
    const bool bCreateAux = CPLFetchBool(papszOptions, "AUX", false);

    char *pszFullFilename = nullptr;
    char *pszRawFilename = nullptr;

    // Work out some details about the tiling scheme.
    const int nBlocksPerRow = DIV_ROUND_UP(nXSize, nBlockSize);
    const int nBlocksPerColumn = DIV_ROUND_UP(nYSize, nBlockSize);
    if (nBlocksPerRow > INT_MAX / nBlocksPerColumn)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Too many blocks");
        return nullptr;
    }
    const int nBlocks = nBlocksPerRow * nBlocksPerColumn;
    const GInt64 nBytesPerBlock64 =
        (static_cast<GInt64>(nBlockSize) * nBlockSize *
             HFAGetDataTypeBits(eDataType) +
         7) /
        8;
    if (nBytesPerBlock64 > INT_MAX)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Too large block");
        return nullptr;
    }
    const int nBytesPerBlock = static_cast<int>(nBytesPerBlock64);

    // Create the low level structure.
    HFAHandle psInfo = HFACreateLL(pszFilename);
    if (psInfo == nullptr)
        return nullptr;

    // Create the DependentFile node if requested.
    const char *pszDependentFile =
        CSLFetchNameValue(papszOptions, "DEPENDENT_FILE");

    if (pszDependentFile != nullptr)
    {
        HFAEntry *poDF = HFAEntry::New(psInfo, "DependentFile",
                                       "Eimg_DependentFile", psInfo->poRoot);

        poDF->MakeData(static_cast<int>(strlen(pszDependentFile) + 50));
        poDF->SetPosition();
        poDF->SetStringField("dependent.string", pszDependentFile);
    }

    CPLDebug("HFACreate",
             "Blocks per row %d, blocks per column %d, "
             "total number of blocks %d, bytes per block %d.",
             nBlocksPerRow, nBlocksPerColumn, nBlocks, nBytesPerBlock);

    // Check whether we should create external large file with
    // image.  We create a spill file if the amount of imagery is
    // close to 2GB.  We don't check the amount of auxiliary
    // information, so in theory if there were an awful lot of
    // non-imagery data our approximate size could be smaller than
    // the file will actually we be.  We leave room for 10MB of
    // auxiliary data.
    // We can also force spill file creation using option
    // SPILL_FILE=YES.
    const double dfApproxSize = static_cast<double>(nBytesPerBlock) *
                                    static_cast<double>(nBlocks) *
                                    static_cast<double>(nBands) +
                                10000000.0;

    if (dfApproxSize > 2147483648.0 && !bCreateAux)
        bCreateLargeRaster = true;

    // Erdas Imagine creates this entry even if an external spill file is used.
    if (!bCreateAux)
    {
        HFAEntry *poImgFormat = HFAEntry::New(
            psInfo, "IMGFormatInfo", "ImgFormatInfo831", psInfo->poRoot);
        poImgFormat->MakeData();
        if (bCreateLargeRaster)
        {
            poImgFormat->SetIntField("spaceUsedForRasterData", 0);
            // Can't be compressed if we are creating a spillfile.
            bCreateCompressed = false;
        }
        else
        {
            poImgFormat->SetIntField("spaceUsedForRasterData",
                                     nBytesPerBlock * nBlocks * nBands);
        }
    }

    // Create external file and write its header.
    GIntBig nValidFlagsOffset = 0;
    GIntBig nDataOffset = 0;

    if (bCreateLargeRaster)
    {
        if (!HFACreateSpillStack(psInfo, nXSize, nYSize, nBands, nBlockSize,
                                 eDataType, &nValidFlagsOffset, &nDataOffset))
        {
            CPLFree(pszRawFilename);
            CPLFree(pszFullFilename);
            return nullptr;
        }
    }

    // Create each band (layer).
    for (int iBand = 0; iBand < nBands; iBand++)
    {
        char szName[128] = {};

        snprintf(szName, sizeof(szName), "Layer_%d", iBand + 1);

        if (!HFACreateLayer(psInfo, psInfo->poRoot, szName, FALSE, nBlockSize,
                            bCreateCompressed, bCreateLargeRaster, bCreateAux,
                            nXSize, nYSize, eDataType, papszOptions,
                            nValidFlagsOffset, nDataOffset, nBands, iBand))
        {
            CPL_IGNORE_RET_VAL(HFAClose(psInfo));
            return nullptr;
        }
    }

    // Initialize the band information.
    HFAParseBandInfo(psInfo);

    return psInfo;
}

/************************************************************************/
/*                         HFACreateOverview()                          */
/*                                                                      */
/*      Create an overview layer object for a band.                     */
/************************************************************************/

int HFACreateOverview(HFAHandle hHFA, int nBand, int nOverviewLevel,
                      const char *pszResampling)

{
    if (nBand < 1 || nBand > hHFA->nBands)
        return -1;

    HFABand *poBand = hHFA->papoBand[nBand - 1];
    return poBand->CreateOverview(nOverviewLevel, pszResampling);
}

/************************************************************************/
/*                           HFAGetMetadata()                           */
/*                                                                      */
/*      Read metadata structured in a table called GDAL_MetaData.       */
/************************************************************************/

char **HFAGetMetadata(HFAHandle hHFA, int nBand)

{
    HFAEntry *poTable = nullptr;

    if (nBand > 0 && nBand <= hHFA->nBands)
        poTable = hHFA->papoBand[nBand - 1]->poNode->GetChild();
    else if (nBand == 0)
        poTable = hHFA->poRoot->GetChild();
    else
        return nullptr;

    for (; poTable != nullptr && !EQUAL(poTable->GetName(), "GDAL_MetaData");
         poTable = poTable->GetNext())
    {
    }

    if (poTable == nullptr || !EQUAL(poTable->GetType(), "Edsc_Table"))
        return nullptr;

    if (poTable->GetIntField("numRows") != 1)
    {
        CPLDebug("HFADataset", "GDAL_MetaData.numRows = %d, expected 1!",
                 poTable->GetIntField("numRows"));
        return nullptr;
    }

    // Loop over each column.  Each column will be one metadata
    // entry, with the title being the key, and the row value being
    // the value.  There is only ever one row in GDAL_MetaData tables.
    char **papszMD = nullptr;

    for (HFAEntry *poColumn = poTable->GetChild(); poColumn != nullptr;
         poColumn = poColumn->GetNext())
    {
        // Skip the #Bin_Function# entry.
        if (STARTS_WITH_CI(poColumn->GetName(), "#"))
            continue;

        const char *pszValue = poColumn->GetStringField("dataType");
        if (pszValue == nullptr || !EQUAL(pszValue, "string"))
            continue;

        const int columnDataPtr = poColumn->GetIntField("columnDataPtr");
        if (columnDataPtr <= 0)
            continue;

        // Read up to nMaxNumChars bytes from the indicated location.
        // allocate required space temporarily nMaxNumChars should have been
        // set by GDAL originally so we should trust it, but who knows.
        const int nMaxNumChars = poColumn->GetIntField("maxNumChars");

        if (nMaxNumChars <= 0)
        {
            papszMD = CSLSetNameValue(papszMD, poColumn->GetName(), "");
        }
        else
        {
            char *pszMDValue =
                static_cast<char *>(VSI_MALLOC_VERBOSE(nMaxNumChars));
            if (pszMDValue == nullptr)
            {
                continue;
            }

            if (VSIFSeekL(hHFA->fp, columnDataPtr, SEEK_SET) != 0)
            {
                CPLFree(pszMDValue);
                continue;
            }

            const int nMDBytes = static_cast<int>(
                VSIFReadL(pszMDValue, 1, nMaxNumChars, hHFA->fp));
            if (nMDBytes == 0)
            {
                CPLFree(pszMDValue);
                continue;
            }

            pszMDValue[nMaxNumChars - 1] = '\0';

            papszMD = CSLSetNameValue(papszMD, poColumn->GetName(), pszMDValue);
            CPLFree(pszMDValue);
        }
    }

    return papszMD;
}

/************************************************************************/
/*                         HFASetGDALMetadata()                         */
/*                                                                      */
/*      This function is used to set metadata in a table called         */
/*      GDAL_MetaData.  It is called by HFASetMetadata() for all        */
/*      metadata items that aren't some specific supported              */
/*      information (like histogram or stats info).                     */
/************************************************************************/

static CPLErr HFASetGDALMetadata(HFAHandle hHFA, int nBand, char **papszMD)

{
    if (papszMD == nullptr)
        return CE_None;

    HFAEntry *poNode = nullptr;

    if (nBand > 0 && nBand <= hHFA->nBands)
        poNode = hHFA->papoBand[nBand - 1]->poNode;
    else if (nBand == 0)
        poNode = hHFA->poRoot;
    else
        return CE_Failure;

    // Create the Descriptor table.
    // Check we have no table with this name already.
    HFAEntry *poEdsc_Table = poNode->GetNamedChild("GDAL_MetaData");

    if (poEdsc_Table == nullptr ||
        !EQUAL(poEdsc_Table->GetType(), "Edsc_Table"))
        poEdsc_Table =
            HFAEntry::New(hHFA, "GDAL_MetaData", "Edsc_Table", poNode);

    poEdsc_Table->SetIntField("numrows", 1);

    // Create the Binning function node.  Do we really need this though?
    // Check it doesn't exist already.
    HFAEntry *poEdsc_BinFunction =
        poEdsc_Table->GetNamedChild("#Bin_Function#");

    if (poEdsc_BinFunction == nullptr ||
        !EQUAL(poEdsc_BinFunction->GetType(), "Edsc_BinFunction"))
        poEdsc_BinFunction = HFAEntry::New(hHFA, "#Bin_Function#",
                                           "Edsc_BinFunction", poEdsc_Table);

    // Because of the BaseData we have to hardcode the size.
    poEdsc_BinFunction->MakeData(30);

    poEdsc_BinFunction->SetIntField("numBins", 1);
    poEdsc_BinFunction->SetStringField("binFunction", "direct");
    poEdsc_BinFunction->SetDoubleField("minLimit", 0.0);
    poEdsc_BinFunction->SetDoubleField("maxLimit", 0.0);

    // Process each metadata item as a separate column.
    bool bRet = true;
    for (int iColumn = 0; papszMD[iColumn] != nullptr; iColumn++)
    {
        char *pszKey = nullptr;
        const char *pszValue = CPLParseNameValue(papszMD[iColumn], &pszKey);
        if (pszValue == nullptr)
            continue;

        // Create the Edsc_Column.
        // Check it doesn't exist already.
        HFAEntry *poEdsc_Column = poEdsc_Table->GetNamedChild(pszKey);

        if (poEdsc_Column == nullptr ||
            !EQUAL(poEdsc_Column->GetType(), "Edsc_Column"))
            poEdsc_Column =
                HFAEntry::New(hHFA, pszKey, "Edsc_Column", poEdsc_Table);

        poEdsc_Column->SetIntField("numRows", 1);
        poEdsc_Column->SetStringField("dataType", "string");
        poEdsc_Column->SetIntField("maxNumChars",
                                   static_cast<GUInt32>(strlen(pszValue) + 1));

        // Write the data out.
        const int nOffset =
            HFAAllocateSpace(hHFA, static_cast<GUInt32>(strlen(pszValue) + 1));

        poEdsc_Column->SetIntField("columnDataPtr", nOffset);

        bRet &= VSIFSeekL(hHFA->fp, nOffset, SEEK_SET) >= 0;
        bRet &=
            VSIFWriteL((void *)pszValue, strlen(pszValue) + 1, 1, hHFA->fp) > 0;

        CPLFree(pszKey);
    }

    return bRet ? CE_None : CE_Failure;
}

/************************************************************************/
/*                           HFASetMetadata()                           */
/************************************************************************/

CPLErr HFASetMetadata(HFAHandle hHFA, int nBand, char **papszMD)

{
    char **papszGDALMD = nullptr;

    if (CSLCount(papszMD) == 0)
        return CE_None;

    HFAEntry *poNode = nullptr;

    if (nBand > 0 && nBand <= hHFA->nBands)
        poNode = hHFA->papoBand[nBand - 1]->poNode;
    else if (nBand == 0)
        poNode = hHFA->poRoot;
    else
        return CE_Failure;
#ifdef DEBUG
    // To please Clang Static Analyzer (CSA).
    if (poNode == nullptr)
    {
        CPLAssert(false);
        return CE_Failure;
    }
#endif

    // Check if the Metadata is an "known" entity which should be
    // stored in a better place.
    char *pszBinValues = nullptr;
    bool bCreatedHistogramParameters = false;
    bool bCreatedStatistics = false;
    const char *const *pszAuxMetaData = GetHFAAuxMetaDataList();
    // Check each metadata item.
    for (int iColumn = 0; papszMD[iColumn] != nullptr; iColumn++)
    {
        char *pszKey = nullptr;
        const char *pszValue = CPLParseNameValue(papszMD[iColumn], &pszKey);
        if (pszValue == nullptr)
            continue;

        // Know look if its known.
        int i = 0;  // Used after for.
        for (; pszAuxMetaData[i] != nullptr; i += 4)
        {
            if (EQUALN(pszAuxMetaData[i + 2], pszKey, strlen(pszKey)))
                break;
        }
        if (pszAuxMetaData[i] != nullptr)
        {
            // Found one, get the right entry.
            HFAEntry *poEntry = nullptr;

            if (strlen(pszAuxMetaData[i]) > 0)
                poEntry = poNode->GetNamedChild(pszAuxMetaData[i]);
            else
                poEntry = poNode;

            if (poEntry == nullptr && strlen(pszAuxMetaData[i + 3]) > 0)
            {
                // Child does not yet exist --> create it,
                poEntry = HFAEntry::New(hHFA, pszAuxMetaData[i],
                                        pszAuxMetaData[i + 3], poNode);

                if (STARTS_WITH_CI(pszAuxMetaData[i], "Statistics"))
                    bCreatedStatistics = true;

                if (STARTS_WITH_CI(pszAuxMetaData[i], "HistogramParameters"))
                {
                    // A bit nasty.  Need to set the string field for the object
                    // first because the SetStringField sets the count for the
                    // object BinFunction to the length of the string.
                    poEntry->MakeData(70);
                    poEntry->SetStringField("BinFunction.binFunctionType",
                                            "direct");

                    bCreatedHistogramParameters = true;
                }
            }
            if (poEntry == nullptr)
            {
                CPLFree(pszKey);
                continue;
            }

            const char *pszFieldName = pszAuxMetaData[i + 1] + 1;
            switch (pszAuxMetaData[i + 1][0])
            {
                case 'd':
                {
                    double dfValue = CPLAtof(pszValue);
                    poEntry->SetDoubleField(pszFieldName, dfValue);
                }
                break;
                case 'i':
                case 'l':
                {
                    int nValue = atoi(pszValue);
                    poEntry->SetIntField(pszFieldName, nValue);
                }
                break;
                case 's':
                case 'e':
                {
                    poEntry->SetStringField(pszFieldName, pszValue);
                }
                break;
                default:
                    CPLAssert(false);
            }
        }
        else if (STARTS_WITH_CI(pszKey, "STATISTICS_HISTOBINVALUES"))
        {
            CPLFree(pszBinValues);
            pszBinValues = CPLStrdup(pszValue);
        }
        else
        {
            papszGDALMD = CSLAddString(papszGDALMD, papszMD[iColumn]);
        }

        CPLFree(pszKey);
    }

    // Special case to write out the histogram.
    bool bRet = true;
    if (pszBinValues != nullptr)
    {
        HFAEntry *poEntry = poNode->GetNamedChild("HistogramParameters");
        if (poEntry != nullptr && bCreatedHistogramParameters)
        {
            // If this node exists we have added Histogram data -- complete with
            // some defaults.
            poEntry->SetIntField("SkipFactorX", 1);
            poEntry->SetIntField("SkipFactorY", 1);

            const int nNumBins = poEntry->GetIntField("BinFunction.numBins");
            const double dMinLimit =
                poEntry->GetDoubleField("BinFunction.minLimit");
            const double dMaxLimit =
                poEntry->GetDoubleField("BinFunction.maxLimit");

            // Fill the descriptor table - check it isn't there already.
            poEntry = poNode->GetNamedChild("Descriptor_Table");
            if (poEntry == nullptr || !EQUAL(poEntry->GetType(), "Edsc_Table"))
                poEntry = HFAEntry::New(hHFA, "Descriptor_Table", "Edsc_Table",
                                        poNode);

            poEntry->SetIntField("numRows", nNumBins);

            // Bin function.
            HFAEntry *poBinFunc = poEntry->GetNamedChild("#Bin_Function#");
            if (poBinFunc == nullptr ||
                !EQUAL(poBinFunc->GetType(), "Edsc_BinFunction"))
                poBinFunc = HFAEntry::New(hHFA, "#Bin_Function#",
                                          "Edsc_BinFunction", poEntry);

            poBinFunc->MakeData(30);
            poBinFunc->SetIntField("numBins", nNumBins);
            poBinFunc->SetDoubleField("minLimit", dMinLimit);
            poBinFunc->SetDoubleField("maxLimit", dMaxLimit);
            // Direct for thematic layers, linear otherwise.
            if (STARTS_WITH_CI(poNode->GetStringField("layerType"), "thematic"))
                poBinFunc->SetStringField("binFunctionType", "direct");
            else
                poBinFunc->SetStringField("binFunctionType", "linear");

            // We need a child named histogram.
            HFAEntry *poHisto = poEntry->GetNamedChild("Histogram");
            if (poHisto == nullptr || !EQUAL(poHisto->GetType(), "Edsc_Column"))
                poHisto =
                    HFAEntry::New(hHFA, "Histogram", "Edsc_Column", poEntry);

            poHisto->SetIntField("numRows", nNumBins);
            // Allocate space for the bin values.
            GUInt32 nOffset = HFAAllocateSpace(hHFA, nNumBins * 8);
            poHisto->SetIntField("columnDataPtr", nOffset);
            poHisto->SetStringField("dataType", "real");
            poHisto->SetIntField("maxNumChars", 0);
            // Write out histogram data.
            char *pszWork = pszBinValues;
            for (int nBin = 0; nBin < nNumBins; ++nBin)
            {
                char *pszEnd = strchr(pszWork, '|');
                if (pszEnd != nullptr)
                {
                    *pszEnd = 0;
                    bRet &=
                        VSIFSeekL(hHFA->fp, nOffset + 8 * nBin, SEEK_SET) >= 0;
                    double nValue = CPLAtof(pszWork);
                    HFAStandard(8, &nValue);

                    bRet &= VSIFWriteL((void *)&nValue, 8, 1, hHFA->fp) > 0;
                    pszWork = pszEnd + 1;
                }
            }
        }
        else if (poEntry != nullptr)
        {
            // In this case, there are HistogramParameters present, but we did
            // not create them. However, we might be modifying them, in the case
            // where the data has changed and the histogram counts need to be
            // updated. It could be worse than that, but that is all we are
            // going to cope with for now.  We are assuming that we did not
            // change any of the other stuff, like skip factors and so
            // forth. The main need for this case is for programs (such as
            // Imagine itself) which will happily modify the pixel values
            // without re-calculating the histogram counts.
            int nNumBins = poEntry->GetIntField("BinFunction.numBins");
            HFAEntry *poEntryDescrTbl =
                poNode->GetNamedChild("Descriptor_Table");
            HFAEntry *poHisto = nullptr;
            if (poEntryDescrTbl != nullptr)
            {
                poHisto = poEntryDescrTbl->GetNamedChild("Histogram");
            }
            if (poHisto != nullptr)
            {
                int nOffset = poHisto->GetIntField("columnDataPtr");
                // Write out histogram data.
                char *pszWork = pszBinValues;

                // Check whether histogram counts were written as int or double
                bool bCountIsInt = true;
                const char *pszDataType = poHisto->GetStringField("dataType");
                if (STARTS_WITH_CI(pszDataType, "real"))
                {
                    bCountIsInt = false;
                }
                for (int nBin = 0; nBin < nNumBins; ++nBin)
                {
                    char *pszEnd = strchr(pszWork, '|');
                    if (pszEnd != nullptr)
                    {
                        *pszEnd = 0;
                        if (bCountIsInt)
                        {
                            // Histogram counts were written as ints, so
                            // re-write them the same way.
                            bRet &= VSIFSeekL(hHFA->fp, nOffset + 4 * nBin,
                                              SEEK_SET) >= 0;
                            int nValue = atoi(pszWork);
                            HFAStandard(4, &nValue);
                            bRet &=
                                VSIFWriteL((void *)&nValue, 4, 1, hHFA->fp) > 0;
                        }
                        else
                        {
                            // Histogram were written as doubles, as is now the
                            // default behavior.
                            bRet &= VSIFSeekL(hHFA->fp, nOffset + 8 * nBin,
                                              SEEK_SET) >= 0;
                            double nValue = CPLAtof(pszWork);
                            HFAStandard(8, &nValue);
                            bRet &=
                                VSIFWriteL((void *)&nValue, 8, 1, hHFA->fp) > 0;
                        }
                        pszWork = pszEnd + 1;
                    }
                }
            }
        }
        CPLFree(pszBinValues);
    }

    // If we created a statistics node then try to create a
    // StatisticsParameters node too.
    if (bCreatedStatistics)
    {
        HFAEntry *poEntry =
            HFAEntry::New(hHFA, "StatisticsParameters",
                          "Eimg_StatisticsParameters830", poNode);

        poEntry->MakeData(70);
        // poEntry->SetStringField( "BinFunction.binFunctionType", "linear" );

        poEntry->SetIntField("SkipFactorX", 1);
        poEntry->SetIntField("SkipFactorY", 1);
    }

    // Write out metadata items without a special place.
    if (bRet && CSLCount(papszGDALMD) != 0)
    {
        CPLErr eErr = HFASetGDALMetadata(hHFA, nBand, papszGDALMD);

        CSLDestroy(papszGDALMD);
        return eErr;
    }
    else
    {
        CSLDestroy(papszGDALMD);
        return CE_Failure;
    }
}

/************************************************************************/
/*                         HFAGetIGEFilename()                          */
/*                                                                      */
/*      Returns the .ige filename if one is associated with this        */
/*      object.  For files not newly created we need to scan the        */
/*      bands for spill files.  Presumably there will only be one.      */
/*                                                                      */
/*      NOTE: Returns full path, not just the filename portion.         */
/************************************************************************/

std::string HFAGetIGEFilename(HFAHandle hHFA)

{
    if (hHFA->pszIGEFilename == nullptr)
    {
        std::vector<HFAEntry *> apoDMSList =
            hHFA->poRoot->FindChildren(nullptr, "ImgExternalRaster");

        HFAEntry *poDMS = apoDMSList.empty() ? nullptr : apoDMSList[0];

        // Get the IGE filename from if we have an ExternalRasterDMS.
        if (poDMS)
        {
            const char *pszRawFilename =
                poDMS->GetStringField("fileName.string");

            if (pszRawFilename != nullptr)
            {
                VSIStatBufL sStatBuf;
                std::string osFullFilename =
                    CPLFormFilenameSafe(hHFA->pszPath, pszRawFilename, nullptr);

                if (VSIStatL(osFullFilename.c_str(), &sStatBuf) != 0)
                {
                    const CPLString osExtension =
                        CPLGetExtensionSafe(pszRawFilename);
                    const CPLString osBasename =
                        CPLGetBasenameSafe(hHFA->pszFilename);
                    osFullFilename = CPLFormFilenameSafe(
                        hHFA->pszPath, osBasename, osExtension);

                    if (VSIStatL(osFullFilename.c_str(), &sStatBuf) == 0)
                        hHFA->pszIGEFilename =
                            CPLStrdup(CPLFormFilenameSafe(nullptr, osBasename,
                                                          osExtension)
                                          .c_str());
                    else
                        hHFA->pszIGEFilename = CPLStrdup(pszRawFilename);
                }
                else
                {
                    hHFA->pszIGEFilename = CPLStrdup(pszRawFilename);
                }
            }
        }
    }

    // Return the full filename.
    if (hHFA->pszIGEFilename)
        return CPLFormFilenameSafe(hHFA->pszPath, hHFA->pszIGEFilename,
                                   nullptr);

    return std::string();
}

/************************************************************************/
/*                        HFACreateSpillStack()                         */
/*                                                                      */
/*      Create a new stack of raster layers in the spill (.ige)         */
/*      file.  Create the spill file if it didn't exist before.         */
/************************************************************************/

bool HFACreateSpillStack(HFAInfo_t *psInfo, int nXSize, int nYSize, int nLayers,
                         int nBlockSize, EPTType eDataType,
                         GIntBig *pnValidFlagsOffset, GIntBig *pnDataOffset)

{
    // Form .ige filename.
    if (nBlockSize <= 0)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "HFACreateSpillStack: nBlockXSize < 0");
        return false;
    }

    if (psInfo->pszIGEFilename == nullptr)
    {
        const auto osExt = CPLGetExtensionSafe(psInfo->pszFilename);
        if (EQUAL(osExt.c_str(), "rrd"))
            psInfo->pszIGEFilename = CPLStrdup(
                CPLResetExtensionSafe(psInfo->pszFilename, "rde").c_str());
        else if (EQUAL(osExt.c_str(), "aux"))
            psInfo->pszIGEFilename = CPLStrdup(
                CPLResetExtensionSafe(psInfo->pszFilename, "axe").c_str());
        else
            psInfo->pszIGEFilename = CPLStrdup(
                CPLResetExtensionSafe(psInfo->pszFilename, "ige").c_str());
    }

    char *pszFullFilename = CPLStrdup(
        CPLFormFilenameSafe(psInfo->pszPath, psInfo->pszIGEFilename, nullptr)
            .c_str());

    // Try and open it.  If we fail, create it and write the magic header.
    static const char *const pszMagick = "ERDAS_IMG_EXTERNAL_RASTER";

    bool bRet = true;
    VSILFILE *fpVSIL = VSIFOpenL(pszFullFilename, "r+b");
    if (fpVSIL == nullptr)
    {
        fpVSIL = VSIFOpenL(pszFullFilename, "w+");
        if (fpVSIL == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to create spill file %s.\n%s",
                     psInfo->pszIGEFilename, VSIStrerror(errno));
            return false;
        }

        bRet &=
            VSIFWriteL((void *)pszMagick, strlen(pszMagick) + 1, 1, fpVSIL) > 0;
    }

    CPLFree(pszFullFilename);

    // Work out some details about the tiling scheme.
    const int nBlocksPerRow = DIV_ROUND_UP(nXSize, nBlockSize);
    const int nBlocksPerColumn = DIV_ROUND_UP(nYSize, nBlockSize);
    // const int nBlocks = nBlocksPerRow * nBlocksPerColumn;
    const int nBytesPerBlock =
        (nBlockSize * nBlockSize * HFAGetDataTypeBits(eDataType) + 7) / 8;

    const int nBytesPerRow = (nBlocksPerRow + 7) / 8;
    const int nBlockMapSize = nBytesPerRow * nBlocksPerColumn;
    // const int iFlagsSize = nBlockMapSize + 20;

    // Write stack prefix information.
    bRet &= VSIFSeekL(fpVSIL, 0, SEEK_END) >= 0;

    GByte bUnknown = 1;
    bRet &= VSIFWriteL(&bUnknown, 1, 1, fpVSIL) > 0;

    GInt32 nValue32 = nLayers;
    HFAStandard(4, &nValue32);
    bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
    nValue32 = nXSize;
    HFAStandard(4, &nValue32);
    bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
    nValue32 = nYSize;
    HFAStandard(4, &nValue32);
    bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
    nValue32 = nBlockSize;
    HFAStandard(4, &nValue32);
    bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
    bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
    bUnknown = 3;
    bRet &= VSIFWriteL(&bUnknown, 1, 1, fpVSIL) > 0;
    bUnknown = 0;
    bRet &= VSIFWriteL(&bUnknown, 1, 1, fpVSIL) > 0;

    // Write out ValidFlags section(s).
    *pnValidFlagsOffset = VSIFTellL(fpVSIL);

    unsigned char *pabyBlockMap =
        static_cast<unsigned char *>(VSI_MALLOC_VERBOSE(nBlockMapSize));
    if (pabyBlockMap == nullptr)
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fpVSIL));
        return false;
    }

    memset(pabyBlockMap, 0xff, nBlockMapSize);
    for (int iBand = 0; iBand < nLayers; iBand++)
    {
        nValue32 = 1;  // Unknown
        HFAStandard(4, &nValue32);
        bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
        nValue32 = 0;  // Unknown
        bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
        nValue32 = nBlocksPerColumn;
        HFAStandard(4, &nValue32);
        bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
        nValue32 = nBlocksPerRow;
        HFAStandard(4, &nValue32);
        bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;
        nValue32 = 0x30000;  // Unknown
        HFAStandard(4, &nValue32);
        bRet &= VSIFWriteL(&nValue32, 4, 1, fpVSIL) > 0;

        const int iRemainder = nBlocksPerRow % 8;
        CPLDebug("HFACreate",
                 "Block map size %d, bytes per row %d, remainder %d.",
                 nBlockMapSize, nBytesPerRow, iRemainder);
        if (iRemainder)
        {
            for (int i = nBytesPerRow - 1; i < nBlockMapSize; i += nBytesPerRow)
                pabyBlockMap[i] = static_cast<GByte>((1 << iRemainder) - 1);
        }

        bRet &= VSIFWriteL(pabyBlockMap, nBlockMapSize, 1, fpVSIL) > 0;
    }
    CPLFree(pabyBlockMap);
    pabyBlockMap = nullptr;

    // Extend the file to account for all the imagery space.
    const GIntBig nTileDataSize = static_cast<GIntBig>(nBytesPerBlock) *
                                  nBlocksPerRow * nBlocksPerColumn * nLayers;

    *pnDataOffset = VSIFTellL(fpVSIL);

    if (!bRet || VSIFTruncateL(fpVSIL, nTileDataSize + *pnDataOffset) != 0)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Failed to extend %s to full size (" CPL_FRMT_GIB " bytes), "
                 "likely out of disk space.\n%s",
                 psInfo->pszIGEFilename, nTileDataSize + *pnDataOffset,
                 VSIStrerror(errno));

        CPL_IGNORE_RET_VAL(VSIFCloseL(fpVSIL));
        return false;
    }

    if (VSIFCloseL(fpVSIL) != 0)
        return false;

    return true;
}

/************************************************************************/
/*                       HFAReadAndValidatePoly()                       */
/************************************************************************/

static bool HFAReadAndValidatePoly(HFAEntry *poTarget, const char *pszName,
                                   Efga_Polynomial *psRetPoly)

{
    memset(psRetPoly, 0, sizeof(Efga_Polynomial));

    CPLString osFldName;
    osFldName.Printf("%sorder", pszName);
    psRetPoly->order = poTarget->GetIntField(osFldName);

    if (psRetPoly->order < 1 || psRetPoly->order > 3)
        return false;

    // Validate that things are in a "well known" form.
    osFldName.Printf("%snumdimtransform", pszName);
    const int numdimtransform = poTarget->GetIntField(osFldName);

    osFldName.Printf("%snumdimpolynomial", pszName);
    const int numdimpolynomial = poTarget->GetIntField(osFldName);

    osFldName.Printf("%stermcount", pszName);
    const int termcount = poTarget->GetIntField(osFldName);

    if (numdimtransform != 2 || numdimpolynomial != 2)
        return false;

    if ((psRetPoly->order == 1 && termcount != 3) ||
        (psRetPoly->order == 2 && termcount != 6) ||
        (psRetPoly->order == 3 && termcount != 10))
        return false;

    // We don't check the exponent organization for now.  Hopefully
    // it is always standard.

    // Get coefficients.
    for (int i = 0; i < termcount * 2 - 2; i++)
    {
        osFldName.Printf("%spolycoefmtx[%d]", pszName, i);
        psRetPoly->polycoefmtx[i] = poTarget->GetDoubleField(osFldName);
    }

    for (int i = 0; i < 2; i++)
    {
        osFldName.Printf("%spolycoefvector[%d]", pszName, i);
        psRetPoly->polycoefvector[i] = poTarget->GetDoubleField(osFldName);
    }

    return true;
}

/************************************************************************/
/*                         HFAReadXFormStack()                          */
/************************************************************************/

int HFAReadXFormStack(HFAHandle hHFA, Efga_Polynomial **ppasPolyListForward,
                      Efga_Polynomial **ppasPolyListReverse)

{
    if (hHFA->nBands == 0)
        return 0;

    // Get the HFA node.
    HFAEntry *poXFormHeader =
        hHFA->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm");
    if (poXFormHeader == nullptr)
        return 0;

    // Loop over children, collecting XForms.
    int nStepCount = 0;
    *ppasPolyListForward = nullptr;
    *ppasPolyListReverse = nullptr;

    for (HFAEntry *poXForm = poXFormHeader->GetChild(); poXForm != nullptr;
         poXForm = poXForm->GetNext())
    {
        bool bSuccess = false;
        Efga_Polynomial sForward;
        Efga_Polynomial sReverse;
        memset(&sForward, 0, sizeof(sForward));
        memset(&sReverse, 0, sizeof(sReverse));

        if (EQUAL(poXForm->GetType(), "Efga_Polynomial"))
        {
            bSuccess = HFAReadAndValidatePoly(poXForm, "", &sForward);

            if (bSuccess)
            {
                double adfGT[6] = {
                    sForward.polycoefvector[0], sForward.polycoefmtx[0],
                    sForward.polycoefmtx[2],    sForward.polycoefvector[1],
                    sForward.polycoefmtx[1],    sForward.polycoefmtx[3]};

                double adfInvGT[6] = {};
                bSuccess = HFAInvGeoTransform(adfGT, adfInvGT);
                if (!bSuccess)
                    memset(adfInvGT, 0, sizeof(adfInvGT));

                sReverse.order = sForward.order;
                sReverse.polycoefvector[0] = adfInvGT[0];
                sReverse.polycoefmtx[0] = adfInvGT[1];
                sReverse.polycoefmtx[2] = adfInvGT[2];
                sReverse.polycoefvector[1] = adfInvGT[3];
                sReverse.polycoefmtx[1] = adfInvGT[4];
                sReverse.polycoefmtx[3] = adfInvGT[5];
            }
        }
        else if (EQUAL(poXForm->GetType(), "GM_PolyPair"))
        {
            bSuccess = HFAReadAndValidatePoly(poXForm, "forward.", &sForward) &&
                       HFAReadAndValidatePoly(poXForm, "reverse.", &sReverse);
        }

        if (bSuccess)
        {
            nStepCount++;
            *ppasPolyListForward = static_cast<Efga_Polynomial *>(CPLRealloc(
                *ppasPolyListForward, sizeof(Efga_Polynomial) * nStepCount));
            memcpy(*ppasPolyListForward + nStepCount - 1, &sForward,
                   sizeof(sForward));

            *ppasPolyListReverse = static_cast<Efga_Polynomial *>(CPLRealloc(
                *ppasPolyListReverse, sizeof(Efga_Polynomial) * nStepCount));
            memcpy(*ppasPolyListReverse + nStepCount - 1, &sReverse,
                   sizeof(sReverse));
        }
    }

    return nStepCount;
}

/************************************************************************/
/*                       HFAEvaluateXFormStack()                        */
/************************************************************************/

int HFAEvaluateXFormStack(int nStepCount, int bForward,
                          Efga_Polynomial *pasPolyList, double *pdfX,
                          double *pdfY)

{
    for (int iStep = 0; iStep < nStepCount; iStep++)
    {
        const Efga_Polynomial *psStep =
            bForward ? pasPolyList + iStep
                     : pasPolyList + nStepCount - iStep - 1;

        if (psStep->order == 1)
        {
            const double dfXOut = psStep->polycoefvector[0] +
                                  psStep->polycoefmtx[0] * *pdfX +
                                  psStep->polycoefmtx[2] * *pdfY;

            const double dfYOut = psStep->polycoefvector[1] +
                                  psStep->polycoefmtx[1] * *pdfX +
                                  psStep->polycoefmtx[3] * *pdfY;

            *pdfX = dfXOut;
            *pdfY = dfYOut;
        }
        else if (psStep->order == 2)
        {
            const double dfXOut = psStep->polycoefvector[0] +
                                  psStep->polycoefmtx[0] * *pdfX +
                                  psStep->polycoefmtx[2] * *pdfY +
                                  psStep->polycoefmtx[4] * *pdfX * *pdfX +
                                  psStep->polycoefmtx[6] * *pdfX * *pdfY +
                                  psStep->polycoefmtx[8] * *pdfY * *pdfY;
            const double dfYOut = psStep->polycoefvector[1] +
                                  psStep->polycoefmtx[1] * *pdfX +
                                  psStep->polycoefmtx[3] * *pdfY +
                                  psStep->polycoefmtx[5] * *pdfX * *pdfX +
                                  psStep->polycoefmtx[7] * *pdfX * *pdfY +
                                  psStep->polycoefmtx[9] * *pdfY * *pdfY;

            *pdfX = dfXOut;
            *pdfY = dfYOut;
        }
        else if (psStep->order == 3)
        {
            const double dfXOut =
                psStep->polycoefvector[0] + psStep->polycoefmtx[0] * *pdfX +
                psStep->polycoefmtx[2] * *pdfY +
                psStep->polycoefmtx[4] * *pdfX * *pdfX +
                psStep->polycoefmtx[6] * *pdfX * *pdfY +
                psStep->polycoefmtx[8] * *pdfY * *pdfY +
                psStep->polycoefmtx[10] * *pdfX * *pdfX * *pdfX +
                psStep->polycoefmtx[12] * *pdfX * *pdfX * *pdfY +
                psStep->polycoefmtx[14] * *pdfX * *pdfY * *pdfY +
                psStep->polycoefmtx[16] * *pdfY * *pdfY * *pdfY;
            const double dfYOut =
                psStep->polycoefvector[1] + psStep->polycoefmtx[1] * *pdfX +
                psStep->polycoefmtx[3] * *pdfY +
                psStep->polycoefmtx[5] * *pdfX * *pdfX +
                psStep->polycoefmtx[7] * *pdfX * *pdfY +
                psStep->polycoefmtx[9] * *pdfY * *pdfY +
                psStep->polycoefmtx[11] * *pdfX * *pdfX * *pdfX +
                psStep->polycoefmtx[13] * *pdfX * *pdfX * *pdfY +
                psStep->polycoefmtx[15] * *pdfX * *pdfY * *pdfY +
                psStep->polycoefmtx[17] * *pdfY * *pdfY * *pdfY;

            *pdfX = dfXOut;
            *pdfY = dfYOut;
        }
        else
            return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                         HFAWriteXFormStack()                         */
/************************************************************************/

CPLErr HFAWriteXFormStack(HFAHandle hHFA, int nBand, int nXFormCount,
                          Efga_Polynomial **ppasPolyListForward,
                          Efga_Polynomial **ppasPolyListReverse)

{
    if (nXFormCount == 0)
        return CE_None;

    if (ppasPolyListForward[0]->order != 1)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "For now HFAWriteXFormStack() only supports order 1 polynomials");
        return CE_Failure;
    }

    if (nBand < 0 || nBand > hHFA->nBands)
        return CE_Failure;

    // If no band number is provided, operate on all bands.
    if (nBand == 0)
    {
        for (nBand = 1; nBand <= hHFA->nBands; nBand++)
        {
            CPLErr eErr =
                HFAWriteXFormStack(hHFA, nBand, nXFormCount,
                                   ppasPolyListForward, ppasPolyListReverse);
            if (eErr != CE_None)
                return eErr;
        }

        return CE_None;
    }

    // Fetch our band node.
    HFAEntry *poBandNode = hHFA->papoBand[nBand - 1]->poNode;
    HFAEntry *poXFormHeader = poBandNode->GetNamedChild("MapToPixelXForm");
    if (poXFormHeader == nullptr)
    {
        poXFormHeader = HFAEntry::New(hHFA, "MapToPixelXForm",
                                      "Exfr_GenericXFormHeader", poBandNode);
        poXFormHeader->MakeData(23);
        poXFormHeader->SetPosition();
        poXFormHeader->SetStringField("titleList.string", "Affine");
    }

    // Loop over XForms.
    for (int iXForm = 0; iXForm < nXFormCount; iXForm++)
    {
        Efga_Polynomial *psForward = *ppasPolyListForward + iXForm;
        CPLString osXFormName;
        osXFormName.Printf("XForm%d", iXForm);

        HFAEntry *poXForm = poXFormHeader->GetNamedChild(osXFormName);

        if (poXForm == nullptr)
        {
            poXForm = HFAEntry::New(hHFA, osXFormName, "Efga_Polynomial",
                                    poXFormHeader);
            poXForm->MakeData(136);
            poXForm->SetPosition();
        }

        poXForm->SetIntField("order", 1);
        poXForm->SetIntField("numdimtransform", 2);
        poXForm->SetIntField("numdimpolynomial", 2);
        poXForm->SetIntField("termcount", 3);
        poXForm->SetIntField("exponentlist[0]", 0);
        poXForm->SetIntField("exponentlist[1]", 0);
        poXForm->SetIntField("exponentlist[2]", 1);
        poXForm->SetIntField("exponentlist[3]", 0);
        poXForm->SetIntField("exponentlist[4]", 0);
        poXForm->SetIntField("exponentlist[5]", 1);

        poXForm->SetIntField("polycoefmtx[-3]", EPT_f64);
        poXForm->SetIntField("polycoefmtx[-2]", 2);
        poXForm->SetIntField("polycoefmtx[-1]", 2);
        poXForm->SetDoubleField("polycoefmtx[0]", psForward->polycoefmtx[0]);
        poXForm->SetDoubleField("polycoefmtx[1]", psForward->polycoefmtx[1]);
        poXForm->SetDoubleField("polycoefmtx[2]", psForward->polycoefmtx[2]);
        poXForm->SetDoubleField("polycoefmtx[3]", psForward->polycoefmtx[3]);

        poXForm->SetIntField("polycoefvector[-3]", EPT_f64);
        poXForm->SetIntField("polycoefvector[-2]", 1);
        poXForm->SetIntField("polycoefvector[-1]", 2);
        poXForm->SetDoubleField("polycoefvector[0]",
                                psForward->polycoefvector[0]);
        poXForm->SetDoubleField("polycoefvector[1]",
                                psForward->polycoefvector[1]);
    }

    return CE_None;
}

/************************************************************************/
/*                         HFAReadCameraModel()                         */
/************************************************************************/

char **HFAReadCameraModel(HFAHandle hHFA)

{
    if (hHFA->nBands == 0)
        return nullptr;

    // Get the camera model node, and confirm its type.
    HFAEntry *poXForm =
        hHFA->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm.XForm0");
    if (poXForm == nullptr)
        return nullptr;

    if (!EQUAL(poXForm->GetType(), "Camera_ModelX"))
        return nullptr;

    // Convert the values to metadata.
    char **papszMD = nullptr;
    static const char *const apszFields[] = {"direction",
                                             "refType",
                                             "demsource",
                                             "PhotoDirection",
                                             "RotationSystem",
                                             "demfilename",
                                             "demzunits",
                                             "forSrcAffine[0]",
                                             "forSrcAffine[1]",
                                             "forSrcAffine[2]",
                                             "forSrcAffine[3]",
                                             "forSrcAffine[4]",
                                             "forSrcAffine[5]",
                                             "forDstAffine[0]",
                                             "forDstAffine[1]",
                                             "forDstAffine[2]",
                                             "forDstAffine[3]",
                                             "forDstAffine[4]",
                                             "forDstAffine[5]",
                                             "invSrcAffine[0]",
                                             "invSrcAffine[1]",
                                             "invSrcAffine[2]",
                                             "invSrcAffine[3]",
                                             "invSrcAffine[4]",
                                             "invSrcAffine[5]",
                                             "invDstAffine[0]",
                                             "invDstAffine[1]",
                                             "invDstAffine[2]",
                                             "invDstAffine[3]",
                                             "invDstAffine[4]",
                                             "invDstAffine[5]",
                                             "z_mean",
                                             "lat0",
                                             "lon0",
                                             "coeffs[0]",
                                             "coeffs[1]",
                                             "coeffs[2]",
                                             "coeffs[3]",
                                             "coeffs[4]",
                                             "coeffs[5]",
                                             "coeffs[6]",
                                             "coeffs[7]",
                                             "coeffs[8]",
                                             "LensDistortion[0]",
                                             "LensDistortion[1]",
                                             "LensDistortion[2]",
                                             nullptr};

    const char *pszValue = nullptr;
    for (int i = 0; apszFields[i] != nullptr; i++)
    {
        pszValue = poXForm->GetStringField(apszFields[i]);
        if (pszValue == nullptr)
            pszValue = "";

        papszMD = CSLSetNameValue(papszMD, apszFields[i], pszValue);
    }

    // Create a pseudo-entry for the MIFObject with the outputProjection.
    HFAEntry *poProjInfo =
        HFAEntry::BuildEntryFromMIFObject(poXForm, "outputProjection");
    if (poProjInfo)
    {
        // Fetch the datum.
        Eprj_Datum sDatum;

        memset(&sDatum, 0, sizeof(sDatum));

        sDatum.datumname =
            (char *)poProjInfo->GetStringField("earthModel.datum.datumname");

        const int nDatumType = poProjInfo->GetIntField("earthModel.datum.type");
        if (nDatumType < 0 || nDatumType > EPRJ_DATUM_NONE)
        {
            CPLDebug("HFA", "Invalid value for datum type: %d", nDatumType);
            sDatum.type = EPRJ_DATUM_NONE;
        }
        else
        {
            sDatum.type = static_cast<Eprj_DatumType>(nDatumType);
        }

        for (int i = 0; i < 7; i++)
        {
            char szFieldName[60] = {};

            snprintf(szFieldName, sizeof(szFieldName),
                     "earthModel.datum.params[%d]", i);
            sDatum.params[i] = poProjInfo->GetDoubleField(szFieldName);
        }

        sDatum.gridname =
            (char *)poProjInfo->GetStringField("earthModel.datum.gridname");

        // Fetch the projection parameters.
        Eprj_ProParameters sPro;

        memset(&sPro, 0, sizeof(sPro));

        sPro.proType =
            (Eprj_ProType)poProjInfo->GetIntField("projectionObject.proType");
        sPro.proNumber = poProjInfo->GetIntField("projectionObject.proNumber");
        sPro.proExeName =
            (char *)poProjInfo->GetStringField("projectionObject.proExeName");
        sPro.proName =
            (char *)poProjInfo->GetStringField("projectionObject.proName");
        sPro.proZone = poProjInfo->GetIntField("projectionObject.proZone");

        for (int i = 0; i < 15; i++)
        {
            char szFieldName[40] = {};

            snprintf(szFieldName, sizeof(szFieldName),
                     "projectionObject.proParams[%d]", i);
            sPro.proParams[i] = poProjInfo->GetDoubleField(szFieldName);
        }

        // Fetch the spheroid.
        sPro.proSpheroid.sphereName = (char *)poProjInfo->GetStringField(
            "earthModel.proSpheroid.sphereName");
        sPro.proSpheroid.a =
            poProjInfo->GetDoubleField("earthModel.proSpheroid.a");
        sPro.proSpheroid.b =
            poProjInfo->GetDoubleField("earthModel.proSpheroid.b");
        sPro.proSpheroid.eSquared =
            poProjInfo->GetDoubleField("earthModel.proSpheroid.eSquared");
        sPro.proSpheroid.radius =
            poProjInfo->GetDoubleField("earthModel.proSpheroid.radius");

        // Fetch the projection info.
        // poProjInfo->DumpFieldValues( stdout, "" );

        auto poSRS = HFAPCSStructToOSR(&sDatum, &sPro, nullptr, nullptr);

        if (poSRS)
        {
            char *pszProjection = nullptr;
            if (poSRS->exportToWkt(&pszProjection) == OGRERR_NONE)
            {
                papszMD =
                    CSLSetNameValue(papszMD, "outputProjection", pszProjection);
            }
            CPLFree(pszProjection);
        }

        delete poProjInfo;
    }

    // Fetch the horizontal units.
    pszValue = poXForm->GetStringField("outputHorizontalUnits.string");
    if (pszValue == nullptr)
        pszValue = "";

    papszMD = CSLSetNameValue(papszMD, "outputHorizontalUnits", pszValue);

    // Fetch the elevationinfo.
    HFAEntry *poElevInfo =
        HFAEntry::BuildEntryFromMIFObject(poXForm, "outputElevationInfo");
    if (poElevInfo)
    {
        // poElevInfo->DumpFieldValues( stdout, "" );

        if (poElevInfo->GetDataSize() != 0)
        {
            static const char *const apszEFields[] = {
                "verticalDatum.datumname", "verticalDatum.type",
                "elevationUnit", "elevationType", nullptr};

            for (int i = 0; apszEFields[i] != nullptr; i++)
            {
                pszValue = poElevInfo->GetStringField(apszEFields[i]);
                if (pszValue == nullptr)
                    pszValue = "";

                papszMD = CSLSetNameValue(papszMD, apszEFields[i], pszValue);
            }
        }

        delete poElevInfo;
    }

    return papszMD;
}

/************************************************************************/
/*                         HFAReadElevationUnit()                       */
/************************************************************************/

const char *HFAReadElevationUnit(HFAHandle hHFA, int iBand)
{
    if (hHFA->nBands <= iBand)
        return nullptr;

    HFABand *poBand(hHFA->papoBand[iBand]);
    if (poBand == nullptr || poBand->poNode == nullptr)
    {
        return nullptr;
    }
    HFAEntry *poElevInfo = poBand->poNode->GetNamedChild("Elevation_Info");
    if (poElevInfo == nullptr)
    {
        return nullptr;
    }
    return poElevInfo->GetStringField("elevationUnit");
}

/************************************************************************/
/*                         HFASetGeoTransform()                         */
/*                                                                      */
/*      Set a MapInformation and XForm block.  Allows for rotated       */
/*      and shared geotransforms.                                       */
/************************************************************************/

CPLErr HFASetGeoTransform(HFAHandle hHFA, const char *pszProName,
                          const char *pszUnits, double *padfGeoTransform)

{
    // Write MapInformation.
    for (int nBand = 1; nBand <= hHFA->nBands; nBand++)
    {
        HFAEntry *poBandNode = hHFA->papoBand[nBand - 1]->poNode;

        HFAEntry *poMI = poBandNode->GetNamedChild("MapInformation");
        if (poMI == nullptr)
        {
            poMI = HFAEntry::New(hHFA, "MapInformation", "Eimg_MapInformation",
                                 poBandNode);
            poMI->MakeData(
                static_cast<int>(18 + strlen(pszProName) + strlen(pszUnits)));
            poMI->SetPosition();
        }

        poMI->SetStringField("projection.string", pszProName);
        poMI->SetStringField("units.string", pszUnits);
    }

    // Write XForm.
    double adfAdjTransform[6] = {};

    // Offset by half pixel.

    memcpy(adfAdjTransform, padfGeoTransform, sizeof(double) * 6);
    adfAdjTransform[0] += adfAdjTransform[1] * 0.5;
    adfAdjTransform[0] += adfAdjTransform[2] * 0.5;
    adfAdjTransform[3] += adfAdjTransform[4] * 0.5;
    adfAdjTransform[3] += adfAdjTransform[5] * 0.5;

    // Invert.
    double adfRevTransform[6] = {};
    if (!HFAInvGeoTransform(adfAdjTransform, adfRevTransform))
        memset(adfRevTransform, 0, sizeof(adfRevTransform));

    // Assign to polynomial object.

    Efga_Polynomial sForward;
    memset(&sForward, 0, sizeof(sForward));
    Efga_Polynomial *psForward = &sForward;
    sForward.order = 1;
    sForward.polycoefvector[0] = adfRevTransform[0];
    sForward.polycoefmtx[0] = adfRevTransform[1];
    sForward.polycoefmtx[1] = adfRevTransform[4];
    sForward.polycoefvector[1] = adfRevTransform[3];
    sForward.polycoefmtx[2] = adfRevTransform[2];
    sForward.polycoefmtx[3] = adfRevTransform[5];

    Efga_Polynomial sReverse = sForward;
    Efga_Polynomial *psReverse = &sReverse;

    return HFAWriteXFormStack(hHFA, 0, 1, &psForward, &psReverse);
}

/************************************************************************/
/*                        HFARenameReferences()                         */
/*                                                                      */
/*      Rename references in this .img file from the old basename to    */
/*      a new basename.  This should be passed on to .aux and .rrd      */
/*      files and should include references to .aux, .rrd and .ige.     */
/************************************************************************/

CPLErr HFARenameReferences(HFAHandle hHFA, const char *pszNewBase,
                           const char *pszOldBase)

{
    // Handle RRDNamesList updates.
    std::vector<HFAEntry *> apoNodeList =
        hHFA->poRoot->FindChildren("RRDNamesList", nullptr);

    for (size_t iNode = 0; iNode < apoNodeList.size(); iNode++)
    {
        HFAEntry *poRRDNL = apoNodeList[iNode];
        std::vector<CPLString> aosNL;

        // Collect all the existing names.
        const int nNameCount = poRRDNL->GetFieldCount("nameList");

        CPLString osAlgorithm = poRRDNL->GetStringField("algorithm.string");
        for (int i = 0; i < nNameCount; i++)
        {
            CPLString osFN;
            osFN.Printf("nameList[%d].string", i);
            aosNL.push_back(poRRDNL->GetStringField(osFN));
        }

        // Adjust the names to the new form.
        for (int i = 0; i < nNameCount; i++)
        {
            if (strncmp(aosNL[i], pszOldBase, strlen(pszOldBase)) == 0)
            {
                std::string osNew = pszNewBase;
                osNew += aosNL[i].c_str() + strlen(pszOldBase);
                aosNL[i] = std::move(osNew);
            }
        }

        // Try to make sure the RRDNamesList is big enough to hold the
        // adjusted name list.
        if (strlen(pszNewBase) > strlen(pszOldBase))
        {
            CPLDebug("HFA", "Growing RRDNamesList to hold new names");
            poRRDNL->MakeData(static_cast<int>(
                poRRDNL->GetDataSize() +
                nNameCount * (strlen(pszNewBase) - strlen(pszOldBase))));
        }

        // Initialize the whole thing to zeros for a clean start.
        memset(poRRDNL->GetData(), 0, poRRDNL->GetDataSize());

        // Write the updates back to the file.
        poRRDNL->SetStringField("algorithm.string", osAlgorithm);
        for (int i = 0; i < nNameCount; i++)
        {
            CPLString osFN;
            osFN.Printf("nameList[%d].string", i);
            poRRDNL->SetStringField(osFN, aosNL[i]);
        }
    }

    // Spill file references.
    apoNodeList =
        hHFA->poRoot->FindChildren("ExternalRasterDMS", "ImgExternalRaster");

    for (size_t iNode = 0; iNode < apoNodeList.size(); iNode++)
    {
        HFAEntry *poERDMS = apoNodeList[iNode];

        if (poERDMS == nullptr)
            continue;

        // Fetch all existing values.
        CPLString osFileName = poERDMS->GetStringField("fileName.string");

        GInt32 anValidFlagsOffset[2] = {
            poERDMS->GetIntField("layerStackValidFlagsOffset[0]"),
            poERDMS->GetIntField("layerStackValidFlagsOffset[1]")};

        GInt32 anStackDataOffset[2] = {
            poERDMS->GetIntField("layerStackDataOffset[0]"),
            poERDMS->GetIntField("layerStackDataOffset[1]")};

        const GInt32 nStackCount = poERDMS->GetIntField("layerStackCount");
        const GInt32 nStackIndex = poERDMS->GetIntField("layerStackIndex");

        // Update the filename.
        if (strncmp(osFileName, pszOldBase, strlen(pszOldBase)) == 0)
        {
            std::string osNew = pszNewBase;
            osNew += osFileName.c_str() + strlen(pszOldBase);
            osFileName = std::move(osNew);
        }

        // Grow the node if needed.
        if (strlen(pszNewBase) > strlen(pszOldBase))
        {
            CPLDebug("HFA", "Growing ExternalRasterDMS to hold new names");
            poERDMS->MakeData(
                static_cast<int>(poERDMS->GetDataSize() +
                                 (strlen(pszNewBase) - strlen(pszOldBase))));
        }

        // Initialize the whole thing to zeros for a clean start.
        memset(poERDMS->GetData(), 0, poERDMS->GetDataSize());

        // Write it all out again, this may change the size of the node.
        poERDMS->SetStringField("fileName.string", osFileName);
        poERDMS->SetIntField("layerStackValidFlagsOffset[0]",
                             anValidFlagsOffset[0]);
        poERDMS->SetIntField("layerStackValidFlagsOffset[1]",
                             anValidFlagsOffset[1]);

        poERDMS->SetIntField("layerStackDataOffset[0]", anStackDataOffset[0]);
        poERDMS->SetIntField("layerStackDataOffset[1]", anStackDataOffset[1]);

        poERDMS->SetIntField("layerStackCount", nStackCount);
        poERDMS->SetIntField("layerStackIndex", nStackIndex);
    }

    // DependentFile.
    apoNodeList =
        hHFA->poRoot->FindChildren("DependentFile", "Eimg_DependentFile");

    for (size_t iNode = 0; iNode < apoNodeList.size(); iNode++)
    {
        CPLString osFileName =
            apoNodeList[iNode]->GetStringField("dependent.string");

        // Grow the node if needed.
        if (strlen(pszNewBase) > strlen(pszOldBase))
        {
            CPLDebug("HFA", "Growing DependentFile to hold new names");
            apoNodeList[iNode]->MakeData(
                static_cast<int>(apoNodeList[iNode]->GetDataSize() +
                                 (strlen(pszNewBase) - strlen(pszOldBase))));
        }

        // Update the filename.
        if (strncmp(osFileName, pszOldBase, strlen(pszOldBase)) == 0)
        {
            std::string osNew = pszNewBase;
            osNew += (osFileName.c_str() + strlen(pszOldBase));
            osFileName = std::move(osNew);
        }

        apoNodeList[iNode]->SetStringField("dependent.string", osFileName);
    }

    return CE_None;
}

/* ==================================================================== */
/*      Table relating USGS and ESRI state plane zones.                 */
/* ==================================================================== */
constexpr int anUsgsEsriZones[] = {
    101,  3101, 102,  3126, 201,  3151, 202,  3176, 203,  3201, 301,  3226,
    302,  3251, 401,  3276, 402,  3301, 403,  3326, 404,  3351, 405,  3376,
    406,  3401, 407,  3426, 501,  3451, 502,  3476, 503,  3501, 600,  3526,
    700,  3551, 901,  3601, 902,  3626, 903,  3576, 1001, 3651, 1002, 3676,
    1101, 3701, 1102, 3726, 1103, 3751, 1201, 3776, 1202, 3801, 1301, 3826,
    1302, 3851, 1401, 3876, 1402, 3901, 1501, 3926, 1502, 3951, 1601, 3976,
    1602, 4001, 1701, 4026, 1702, 4051, 1703, 6426, 1801, 4076, 1802, 4101,
    1900, 4126, 2001, 4151, 2002, 4176, 2101, 4201, 2102, 4226, 2103, 4251,
    2111, 6351, 2112, 6376, 2113, 6401, 2201, 4276, 2202, 4301, 2203, 4326,
    2301, 4351, 2302, 4376, 2401, 4401, 2402, 4426, 2403, 4451, 2500, 0,
    2501, 4476, 2502, 4501, 2503, 4526, 2600, 0,    2601, 4551, 2602, 4576,
    2701, 4601, 2702, 4626, 2703, 4651, 2800, 4676, 2900, 4701, 3001, 4726,
    3002, 4751, 3003, 4776, 3101, 4801, 3102, 4826, 3103, 4851, 3104, 4876,
    3200, 4901, 3301, 4926, 3302, 4951, 3401, 4976, 3402, 5001, 3501, 5026,
    3502, 5051, 3601, 5076, 3602, 5101, 3701, 5126, 3702, 5151, 3800, 5176,
    3900, 0,    3901, 5201, 3902, 5226, 4001, 5251, 4002, 5276, 4100, 5301,
    4201, 5326, 4202, 5351, 4203, 5376, 4204, 5401, 4205, 5426, 4301, 5451,
    4302, 5476, 4303, 5501, 4400, 5526, 4501, 5551, 4502, 5576, 4601, 5601,
    4602, 5626, 4701, 5651, 4702, 5676, 4801, 5701, 4802, 5726, 4803, 5751,
    4901, 5776, 4902, 5801, 4903, 5826, 4904, 5851, 5001, 6101, 5002, 6126,
    5003, 6151, 5004, 6176, 5005, 6201, 5006, 6226, 5007, 6251, 5008, 6276,
    5009, 6301, 5010, 6326, 5101, 5876, 5102, 5901, 5103, 5926, 5104, 5951,
    5105, 5976, 5201, 6001, 5200, 6026, 5200, 6076, 5201, 6051, 5202, 6051,
    5300, 0,    5400, 0};

/************************************************************************/
/*                           ESRIToUSGSZone()                           */
/*                                                                      */
/*      Convert ESRI style state plane zones to USGS style state        */
/*      plane zones.                                                    */
/************************************************************************/

static int ESRIToUSGSZone(int nESRIZone)

{
    if (nESRIZone == INT_MIN)
        return 0;
    if (nESRIZone < 0)
        return std::abs(nESRIZone);

    const int nPairs = sizeof(anUsgsEsriZones) / (2 * sizeof(int));
    for (int i = 0; i < nPairs; i++)
    {
        if (anUsgsEsriZones[i * 2 + 1] == nESRIZone)
            return anUsgsEsriZones[i * 2];
    }

    return 0;
}

static const char *const apszDatumMap[] = {
    // Imagine name, WKT name.
    "NAD27",
    "North_American_Datum_1927",
    "NAD83",
    "North_American_Datum_1983",
    "WGS 84",
    "WGS_1984",
    "WGS 1972",
    "WGS_1972",
    "GDA94",
    "Geocentric_Datum_of_Australia_1994",
    "Pulkovo 1942",
    "Pulkovo_1942",
    "Geodetic Datum 1949",
    "New_Zealand_Geodetic_Datum_1949",
    nullptr,
    nullptr};

const char *const *HFAGetDatumMap()
{
    return apszDatumMap;
}

static const char *const apszUnitMap[] = {"meters",
                                          "1.0",
                                          "meter",
                                          "1.0",
                                          "m",
                                          "1.0",
                                          "centimeters",
                                          "0.01",
                                          "centimeter",
                                          "0.01",
                                          "cm",
                                          "0.01",
                                          "millimeters",
                                          "0.001",
                                          "millimeter",
                                          "0.001",
                                          "mm",
                                          "0.001",
                                          "kilometers",
                                          "1000.0",
                                          "kilometer",
                                          "1000.0",
                                          "km",
                                          "1000.0",
                                          "us_survey_feet",
                                          "0.3048006096012192",
                                          "us_survey_foot",
                                          "0.3048006096012192",
                                          "feet",
                                          "0.3048006096012192",
                                          "foot",
                                          "0.3048006096012192",
                                          "ft",
                                          "0.3048006096012192",
                                          "international_feet",
                                          "0.3048",
                                          "international_foot",
                                          "0.3048",
                                          "inches",
                                          "0.0254000508001",
                                          "inch",
                                          "0.0254000508001",
                                          "in",
                                          "0.0254000508001",
                                          "yards",
                                          "0.9144",
                                          "yard",
                                          "0.9144",
                                          "yd",
                                          "0.9144",
                                          "clarke_yard",
                                          "0.9143917962",
                                          "miles",
                                          "1304.544",
                                          "mile",
                                          "1304.544",
                                          "mi",
                                          "1304.544",
                                          "modified_american_feet",
                                          "0.3048122530",
                                          "modified_american_foot",
                                          "0.3048122530",
                                          "clarke_feet",
                                          "0.3047972651",
                                          "clarke_foot",
                                          "0.3047972651",
                                          "indian_feet",
                                          "0.3047995142",
                                          "indian_foot",
                                          "0.3047995142",
                                          nullptr,
                                          nullptr};

const char *const *HFAGetUnitMap()
{
    return apszUnitMap;
}

/************************************************************************/
/*                          HFAPCSStructToOSR()                         */
/*                                                                      */
/*      Convert the datum, proparameters and mapinfo structures into    */
/*      WKT format.                                                     */
/************************************************************************/

std::unique_ptr<OGRSpatialReference>
HFAPCSStructToOSR(const Eprj_Datum *psDatum, const Eprj_ProParameters *psPro,
                  const Eprj_MapInfo *psMapInfo, HFAEntry *poMapInformation)

{
    // General case for Erdas style projections.

    // We make a particular effort to adapt the mapinfo->proname as
    // the PROJCS[] name per #2422.
    auto poSRS = std::make_unique<OGRSpatialReference>();
    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (psPro == nullptr && psMapInfo != nullptr)
    {
        poSRS->SetLocalCS(psMapInfo->proName);
    }
    else if (psPro == nullptr)
    {
        return nullptr;
    }
    else if (psPro->proType == EPRJ_EXTERNAL)
    {
        if (EQUALN(psPro->proExeName, EPRJ_EXTERNAL_NZMG, 4))
        {
            // Handle New Zealand Map Grid (NZMG) external projection.  See:
            // http://www.linz.govt.nz/
            //
            // Is there a better way that doesn't require hardcoding
            // of these numbers?
            poSRS->SetNZMG(-41.0, 173.0, 2510000, 6023150);
        }
        else
        {
            poSRS->SetLocalCS(psPro->proName);
        }
    }
    else if (psPro->proNumber != EPRJ_LATLONG && psMapInfo != nullptr)
    {
        poSRS->SetProjCS(psMapInfo->proName);
    }
    else if (psPro->proNumber != EPRJ_LATLONG)
    {
        poSRS->SetProjCS(psPro->proName);
    }

    // Handle units.  It is important to deal with this first so
    // that the projection Set methods will automatically do
    // translation of linear values (like false easting) to PROJCS
    // units from meters.  Erdas linear projection values are
    // always in meters.
    if (poSRS->IsProjected() || poSRS->IsLocal())
    {
        const char *pszUnits = nullptr;

        if (psMapInfo)
            pszUnits = psMapInfo->units;
        else if (poMapInformation != nullptr)
            pszUnits = poMapInformation->GetStringField("units.string");

        if (pszUnits != nullptr)
        {
            const char *const *papszUnitMap = HFAGetUnitMap();
            int iUnitIndex = 0;  // Used after for.
            for (; papszUnitMap[iUnitIndex] != nullptr; iUnitIndex += 2)
            {
                if (EQUAL(papszUnitMap[iUnitIndex], pszUnits))
                    break;
            }

            if (papszUnitMap[iUnitIndex] == nullptr)
                iUnitIndex = 0;

            poSRS->SetLinearUnits(pszUnits,
                                  CPLAtof(papszUnitMap[iUnitIndex + 1]));
        }
        else
        {
            poSRS->SetLinearUnits(SRS_UL_METER, 1.0);
        }
    }

    if (psPro == nullptr)
    {
        if (poSRS->IsLocal())
        {
            return poSRS;
        }
        else
            return nullptr;
    }

    // Try to work out ellipsoid and datum information.
    const char *pszDatumName = psPro->proSpheroid.sphereName;
    const char *pszEllipsoidName = psPro->proSpheroid.sphereName;

    if (psDatum != nullptr)
    {
        pszDatumName = psDatum->datumname;

        // Imagine to WKT translation.
        const char *const *papszDatumMap = HFAGetDatumMap();
        for (int i = 0; papszDatumMap[i] != nullptr; i += 2)
        {
            if (EQUAL(pszDatumName, papszDatumMap[i]))
            {
                pszDatumName = papszDatumMap[i + 1];
                break;
            }
        }
    }

    if (psPro->proSpheroid.a == 0.0)
        ((Eprj_ProParameters *)psPro)->proSpheroid.a = 6378137.0;
    if (psPro->proSpheroid.b == 0.0)
        ((Eprj_ProParameters *)psPro)->proSpheroid.b = 6356752.3;

    const double dfInvFlattening =
        OSRCalcInvFlattening(psPro->proSpheroid.a, psPro->proSpheroid.b);

    // Handle different projection methods.
    switch (psPro->proNumber)
    {
        case EPRJ_LATLONG:
            break;

        case EPRJ_UTM:
            // We change this to unnamed so that SetUTM will set the long
            // UTM description.
            poSRS->SetProjCS("unnamed");
            poSRS->SetUTM(psPro->proZone, psPro->proParams[3] >= 0.0);

            // The PCS name from the above function may be different with the
            // input name.  If there is a PCS name in psMapInfo that is
            // different with the one in psPro, just use it as the PCS name.
            // This case happens if the dataset's SR was written by the new
            // GDAL.
            if (psMapInfo && strlen(psMapInfo->proName) > 0 &&
                strlen(psPro->proName) > 0 &&
                !EQUAL(psMapInfo->proName, psPro->proName))
                poSRS->SetProjCS(psMapInfo->proName);
            break;

        case EPRJ_STATE_PLANE:
        {
            CPLString osUnitsName;
            double dfLinearUnits;
            {
                const char *pszUnitsName = nullptr;
                dfLinearUnits = poSRS->GetLinearUnits(&pszUnitsName);
                if (pszUnitsName)
                    osUnitsName = pszUnitsName;
            }

            // Historically, hfa used esri state plane zone code. Try esri pe
            // string first.
            const int zoneCode = ESRIToUSGSZone(psPro->proZone);
            const char *pszDatum;
            if (psDatum)
                pszDatum = psDatum->datumname;
            else
                pszDatum = "HARN";
            const char *pszUnits;
            if (psMapInfo)
                pszUnits = psMapInfo->units;
            else if (!osUnitsName.empty())
                pszUnits = osUnitsName;
            else
                pszUnits = "meters";
            const int proNu = psPro->proNumber;
            if (poSRS->ImportFromESRIStatePlaneWKT(zoneCode, pszDatum, pszUnits,
                                                   proNu) == OGRERR_NONE)
            {
                poSRS->AutoIdentifyEPSG();

                return poSRS;
            }

            // Set state plane zone.  Set NAD83/27 on basis of spheroid.
            poSRS->SetStatePlane(ESRIToUSGSZone(psPro->proZone),
                                 fabs(psPro->proSpheroid.a - 6378137.0) < 1.0,
                                 osUnitsName.empty() ? nullptr
                                                     : osUnitsName.c_str(),
                                 dfLinearUnits);

            // Same as the UTM, The following is needed.
            if (psMapInfo && strlen(psMapInfo->proName) > 0 &&
                strlen(psPro->proName) > 0 &&
                !EQUAL(psMapInfo->proName, psPro->proName))
                poSRS->SetProjCS(psMapInfo->proName);
        }
        break;

        case EPRJ_ALBERS_CONIC_EQUAL_AREA:
            poSRS->SetACEA(psPro->proParams[2] * R2D, psPro->proParams[3] * R2D,
                           psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                           psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_LAMBERT_CONFORMAL_CONIC:
            // Check the possible Wisconsin first.
            if (psDatum && psMapInfo && EQUAL(psDatum->datumname, "HARN"))
            {
                // ERO: I doubt this works. Wisconsin LCC is LCC_1SP whereas
                // we are here in the LCC_2SP case...
                if (poSRS->ImportFromESRIWisconsinWKT(
                        "Lambert_Conformal_Conic", psPro->proParams[4] * R2D,
                        psPro->proParams[5] * R2D,
                        psMapInfo->units) == OGRERR_NONE)
                {
                    return poSRS;
                }
            }
            poSRS->SetLCC(psPro->proParams[2] * R2D, psPro->proParams[3] * R2D,
                          psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                          psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_MERCATOR:
            poSRS->SetMercator(psPro->proParams[5] * R2D,
                               psPro->proParams[4] * R2D, 1.0,
                               psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_POLAR_STEREOGRAPHIC:
            poSRS->SetPS(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                         1.0, psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_POLYCONIC:
            poSRS->SetPolyconic(psPro->proParams[5] * R2D,
                                psPro->proParams[4] * R2D, psPro->proParams[6],
                                psPro->proParams[7]);
            break;

        case EPRJ_EQUIDISTANT_CONIC:
        {
            const double dfStdParallel2 = psPro->proParams[8] != 0.0
                                              ? psPro->proParams[3] * R2D
                                              : psPro->proParams[2] * R2D;
            poSRS->SetEC(psPro->proParams[2] * R2D, dfStdParallel2,
                         psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                         psPro->proParams[6], psPro->proParams[7]);
            break;
        }
        case EPRJ_TRANSVERSE_MERCATOR:
        case EPRJ_GAUSS_KRUGER:
            // Check the possible Wisconsin first.
            if (psDatum && psMapInfo && EQUAL(psDatum->datumname, "HARN"))
            {
                if (poSRS->ImportFromESRIWisconsinWKT(
                        "Transverse_Mercator", psPro->proParams[4] * R2D,
                        psPro->proParams[5] * R2D,
                        psMapInfo->units) == OGRERR_NONE)
                {
                    return poSRS;
                }
            }
            poSRS->SetTM(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                         psPro->proParams[2], psPro->proParams[6],
                         psPro->proParams[7]);
            break;

        case EPRJ_STEREOGRAPHIC:
            poSRS->SetStereographic(psPro->proParams[5] * R2D,
                                    psPro->proParams[4] * R2D, 1.0,
                                    psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_LAMBERT_AZIMUTHAL_EQUAL_AREA:
            poSRS->SetLAEA(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                           psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_AZIMUTHAL_EQUIDISTANT:
            poSRS->SetAE(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                         psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_GNOMONIC:
            poSRS->SetGnomonic(psPro->proParams[5] * R2D,
                               psPro->proParams[4] * R2D, psPro->proParams[6],
                               psPro->proParams[7]);
            break;

        case EPRJ_ORTHOGRAPHIC:
            poSRS->SetOrthographic(psPro->proParams[5] * R2D,
                                   psPro->proParams[4] * R2D,
                                   psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_SINUSOIDAL:
            poSRS->SetSinusoidal(psPro->proParams[4] * R2D, psPro->proParams[6],
                                 psPro->proParams[7]);
            break;

        case EPRJ_PLATE_CARREE:
        case EPRJ_EQUIRECTANGULAR:
            poSRS->SetEquirectangular2(
                0.0, psPro->proParams[4] * R2D, psPro->proParams[5] * R2D,
                psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_EQUIDISTANT_CYLINDRICAL:
            poSRS->SetEquirectangular2(
                0.0, psPro->proParams[4] * R2D, psPro->proParams[2] * R2D,
                psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_MILLER_CYLINDRICAL:
            poSRS->SetMC(0.0, psPro->proParams[4] * R2D, psPro->proParams[6],
                         psPro->proParams[7]);
            break;

        case EPRJ_VANDERGRINTEN:
            poSRS->SetVDG(psPro->proParams[4] * R2D, psPro->proParams[6],
                          psPro->proParams[7]);
            break;

        case EPRJ_HOTINE_OBLIQUE_MERCATOR:
            if (psPro->proParams[12] > 0.0)
                poSRS->SetHOM(
                    psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                    psPro->proParams[3] * R2D, 0.0, psPro->proParams[2],
                    psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_HOTINE_OBLIQUE_MERCATOR_AZIMUTH_CENTER:
            poSRS->SetHOMAC(
                psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                psPro->proParams[3] * R2D,
                psPro->proParams[3] *
                    R2D,  // We reuse azimuth as rectified_grid_angle
                psPro->proParams[2], psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_ROBINSON:
            poSRS->SetRobinson(psPro->proParams[4] * R2D, psPro->proParams[6],
                               psPro->proParams[7]);
            break;

        case EPRJ_MOLLWEIDE:
            poSRS->SetMollweide(psPro->proParams[4] * R2D, psPro->proParams[6],
                                psPro->proParams[7]);
            break;

        case EPRJ_GALL_STEREOGRAPHIC:
            poSRS->SetGS(psPro->proParams[4] * R2D, psPro->proParams[6],
                         psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_I:
            poSRS->SetEckert(1, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_II:
            poSRS->SetEckert(2, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_III:
            poSRS->SetEckert(3, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_IV:
            poSRS->SetEckert(4, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_V:
            poSRS->SetEckert(5, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_ECKERT_VI:
            poSRS->SetEckert(6, psPro->proParams[4] * R2D, psPro->proParams[6],
                             psPro->proParams[7]);
            break;

        case EPRJ_CASSINI:
            poSRS->SetCS(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                         psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_TWO_POINT_EQUIDISTANT:
            poSRS->SetTPED(psPro->proParams[9] * R2D, psPro->proParams[8] * R2D,
                           psPro->proParams[11] * R2D,
                           psPro->proParams[10] * R2D, psPro->proParams[6],
                           psPro->proParams[7]);
            break;

        case EPRJ_STEREOGRAPHIC_EXTENDED:
            poSRS->SetStereographic(
                psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                psPro->proParams[2], psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_BONNE:
            poSRS->SetBonne(psPro->proParams[2] * R2D,
                            psPro->proParams[4] * R2D, psPro->proParams[6],
                            psPro->proParams[7]);
            break;

        case EPRJ_LOXIMUTHAL:
        {
            poSRS->SetProjection("Loximuthal");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm("central_parallel",
                                   psPro->proParams[5] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_QUARTIC_AUTHALIC:
        {
            poSRS->SetProjection("Quartic_Authalic");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_WINKEL_I:
        {
            poSRS->SetProjection("Winkel_I");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_STANDARD_PARALLEL_1,
                                   psPro->proParams[2] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_WINKEL_II:
        {
            poSRS->SetProjection("Winkel_II");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_STANDARD_PARALLEL_1,
                                   psPro->proParams[2] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_BEHRMANN:
        {
            poSRS->SetProjection("Behrmann");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_KROVAK:
            poSRS->SetKrovak(
                psPro->proParams[4] * R2D, psPro->proParams[5] * R2D,
                psPro->proParams[3] * R2D, psPro->proParams[9] * R2D,
                psPro->proParams[2], psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_DOUBLE_STEREOGRAPHIC:
        {
            poSRS->SetProjection("Double_Stereographic");
            poSRS->SetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN,
                                   psPro->proParams[5] * R2D);
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_SCALE_FACTOR, psPro->proParams[2]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_AITOFF:
        {
            poSRS->SetProjection("Aitoff");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_CRASTER_PARABOLIC:
        {
            poSRS->SetProjection("Craster_Parabolic");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_CYLINDRICAL_EQUAL_AREA:
            poSRS->SetCEA(psPro->proParams[2] * R2D, psPro->proParams[4] * R2D,
                          psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_FLAT_POLAR_QUARTIC:
        {
            poSRS->SetProjection("Flat_Polar_Quartic");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_TIMES:
        {
            poSRS->SetProjection("Times");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_WINKEL_TRIPEL:
        {
            poSRS->SetProjection("Winkel_Tripel");
            poSRS->SetNormProjParm(SRS_PP_STANDARD_PARALLEL_1,
                                   psPro->proParams[2] * R2D);
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_HAMMER_AITOFF:
        {
            poSRS->SetProjection("Hammer_Aitoff");
            poSRS->SetNormProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                   psPro->proParams[4] * R2D);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_VERTICAL_NEAR_SIDE_PERSPECTIVE:
        {
            poSRS->SetVerticalPerspective(
                psPro->proParams[5] * R2D,  // dfTopoOriginLat
                psPro->proParams[4] * R2D,  // dfTopoOriginLon
                0,                          // dfTopoOriginHeight
                psPro->proParams[2],        // dfViewPointHeight
                psPro->proParams[6],        // dfFalseEasting
                psPro->proParams[7]);       // dfFalseNorthing
        }
        break;

        case EPRJ_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_CENTER:
        {
            poSRS->SetProjection("Hotine_Oblique_Mercator_Twp_Point_Center");
            poSRS->SetNormProjParm(SRS_PP_LATITUDE_OF_CENTER,
                                   psPro->proParams[5] * R2D);
            poSRS->SetNormProjParm(SRS_PP_LATITUDE_OF_1ST_POINT,
                                   psPro->proParams[9] * R2D);
            poSRS->SetNormProjParm(SRS_PP_LONGITUDE_OF_1ST_POINT,
                                   psPro->proParams[8] * R2D);
            poSRS->SetNormProjParm(SRS_PP_LATITUDE_OF_2ND_POINT,
                                   psPro->proParams[11] * R2D);
            poSRS->SetNormProjParm(SRS_PP_LONGITUDE_OF_2ND_POINT,
                                   psPro->proParams[10] * R2D);
            poSRS->SetNormProjParm(SRS_PP_SCALE_FACTOR, psPro->proParams[2]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_EASTING, psPro->proParams[6]);
            poSRS->SetNormProjParm(SRS_PP_FALSE_NORTHING, psPro->proParams[7]);
        }
        break;

        case EPRJ_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN:
            poSRS->SetHOM2PNO(
                psPro->proParams[5] * R2D, psPro->proParams[8] * R2D,
                psPro->proParams[9] * R2D, psPro->proParams[10] * R2D,
                psPro->proParams[11] * R2D, psPro->proParams[2],
                psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_LAMBERT_CONFORMAL_CONIC_1SP:
            poSRS->SetLCC1SP(psPro->proParams[3] * R2D,
                             psPro->proParams[2] * R2D, psPro->proParams[4],
                             psPro->proParams[5], psPro->proParams[6]);
            break;

        case EPRJ_MERCATOR_VARIANT_A:
            poSRS->SetMercator(psPro->proParams[5] * R2D,
                               psPro->proParams[4] * R2D, psPro->proParams[2],
                               psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_PSEUDO_MERCATOR:  // Likely this is google mercator?
            poSRS->SetMercator(psPro->proParams[5] * R2D,
                               psPro->proParams[4] * R2D, 1.0,
                               psPro->proParams[6], psPro->proParams[7]);
            break;

        case EPRJ_HOTINE_OBLIQUE_MERCATOR_VARIANT_A:
            poSRS->SetHOM(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                          psPro->proParams[3] * R2D, psPro->proParams[8] * R2D,
                          psPro->proParams[2], psPro->proParams[6],
                          psPro->proParams[7]);
            break;

        case EPRJ_TRANSVERSE_MERCATOR_SOUTH_ORIENTATED:
            poSRS->SetTMSO(psPro->proParams[5] * R2D, psPro->proParams[4] * R2D,
                           psPro->proParams[2], psPro->proParams[6],
                           psPro->proParams[7]);
            break;

        default:
            if (poSRS->IsProjected())
                poSRS->GetRoot()->SetValue("LOCAL_CS");
            else
                poSRS->SetLocalCS(psPro->proName);
            break;
    }

    // Try and set the GeogCS information.
    if (!poSRS->IsLocal())
    {
        bool bWellKnownDatum = false;
        if (pszDatumName == nullptr)
            poSRS->SetGeogCS(pszDatumName, pszDatumName, pszEllipsoidName,
                             psPro->proSpheroid.a, dfInvFlattening);
        else if (EQUAL(pszDatumName, "WGS 84") ||
                 EQUAL(pszDatumName, "WGS_1984"))
        {
            bWellKnownDatum = true;
            poSRS->SetWellKnownGeogCS("WGS84");
        }
        else if (strstr(pszDatumName, "NAD27") != nullptr ||
                 EQUAL(pszDatumName, "North_American_Datum_1927"))
        {
            bWellKnownDatum = true;
            poSRS->SetWellKnownGeogCS("NAD27");
        }
        else if (EQUAL(pszDatumName, "NAD83") ||
                 EQUAL(pszDatumName, "North_American_Datum_1983"))
        {
            bWellKnownDatum = true;
            poSRS->SetWellKnownGeogCS("NAD83");
        }
        else
        {
            CPLString osGeogCRSName(pszDatumName);

            if (poSRS->IsProjected())
            {
                PJ_CONTEXT *ctxt = OSRGetProjTLSContext();
                const PJ_TYPE type = PJ_TYPE_PROJECTED_CRS;
                PJ_OBJ_LIST *list =
                    proj_create_from_name(ctxt, nullptr, poSRS->GetName(),
                                          &type, 1, false, 1, nullptr);
                if (list)
                {
                    const auto listSize = proj_list_get_count(list);
                    if (listSize == 1)
                    {
                        auto crs = proj_list_get(ctxt, list, 0);
                        if (crs)
                        {
                            auto geogCRS = proj_crs_get_geodetic_crs(ctxt, crs);
                            if (geogCRS)
                            {
                                const char *pszName = proj_get_name(geogCRS);
                                if (pszName)
                                    osGeogCRSName = pszName;
                                proj_destroy(geogCRS);
                            }
                            proj_destroy(crs);
                        }
                    }
                    proj_list_destroy(list);
                }
            }

            poSRS->SetGeogCS(osGeogCRSName, pszDatumName, pszEllipsoidName,
                             psPro->proSpheroid.a, dfInvFlattening);
        }

        if (psDatum != nullptr && psDatum->type == EPRJ_DATUM_PARAMETRIC)
        {
            if (bWellKnownDatum &&
                CPLTestBool(CPLGetConfigOption("OSR_STRIP_TOWGS84", "YES")))
            {
                CPLDebug("OSR",
                         "TOWGS84 information has been removed. "
                         "It can be kept by setting the OSR_STRIP_TOWGS84 "
                         "configuration option to NO");
            }
            else
            {
                poSRS->SetTOWGS84(psDatum->params[0], psDatum->params[1],
                                  psDatum->params[2],
                                  -psDatum->params[3] * RAD2ARCSEC,
                                  -psDatum->params[4] * RAD2ARCSEC,
                                  -psDatum->params[5] * RAD2ARCSEC,
                                  psDatum->params[6] * 1e+6);
                poSRS->StripTOWGS84IfKnownDatumAndAllowed();
            }
        }
    }

    // Try to insert authority information if possible.
    poSRS->AutoIdentifyEPSG();

    auto poSRSBestMatch = poSRS->FindBestMatch(90, nullptr, nullptr);
    if (poSRSBestMatch)
    {
        poSRS.reset(poSRSBestMatch);
    }

    return poSRS;
}
