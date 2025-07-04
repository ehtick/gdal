/******************************************************************************
 *
 * Project:  Arc/Info Binary Grid Driver
 * Purpose:  Implements GDAL interface to underlying library.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "aigrid.h"
#include "avc.h"
#include "cpl_string.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "gdal_rat.h"
#include "ogr_spatialref.h"

#include <vector>

static CPLString OSR_GDS(char **papszNV, const char *pszField,
                         const char *pszDefaultValue);

/************************************************************************/
/* ==================================================================== */
/*                              AIGDataset                              */
/* ==================================================================== */
/************************************************************************/

class AIGRasterBand;

class AIGDataset final : public GDALPamDataset
{
    friend class AIGRasterBand;

    AIGInfo_t *psInfo;

    char **papszPrj;
    OGRSpatialReference m_oSRS{};

    GDALColorTable *poCT;
    bool bHasReadRat;

    void TranslateColorTable(const char *);

    void ReadRAT();
    GDALRasterAttributeTable *poRAT;

  public:
    AIGDataset();
    ~AIGDataset() override;

    static GDALDataset *Open(GDALOpenInfo *);

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    const OGRSpatialReference *GetSpatialRef() const override;
    char **GetFileList(void) override;
};

/************************************************************************/
/* ==================================================================== */
/*                            AIGRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class AIGRasterBand final : public GDALPamRasterBand

{
    friend class AIGDataset;

  public:
    AIGRasterBand(AIGDataset *, int);

    CPLErr IReadBlock(int, int, void *) override;
    double GetMinimum(int *pbSuccess) override;
    double GetMaximum(int *pbSuccess) override;
    double GetNoDataValue(int *pbSuccess) override;

    GDALColorInterp GetColorInterpretation() override;
    GDALColorTable *GetColorTable() override;
    GDALRasterAttributeTable *GetDefaultRAT() override;
};

/************************************************************************/
/*                           AIGRasterBand()                            */
/************************************************************************/

AIGRasterBand::AIGRasterBand(AIGDataset *poDSIn, int nBandIn)

{
    poDS = poDSIn;
    nBand = nBandIn;

    nBlockXSize = poDSIn->psInfo->nBlockXSize;
    nBlockYSize = poDSIn->psInfo->nBlockYSize;

    if (poDSIn->psInfo->nCellType == AIG_CELLTYPE_INT &&
        poDSIn->psInfo->dfMin >= 0.0 && poDSIn->psInfo->dfMax <= 254.0)
    {
        eDataType = GDT_Byte;
    }
    else if (poDSIn->psInfo->nCellType == AIG_CELLTYPE_INT &&
             poDSIn->psInfo->dfMin >= -32767 && poDSIn->psInfo->dfMax <= 32767)
    {
        eDataType = GDT_Int16;
    }
    else if (poDSIn->psInfo->nCellType == AIG_CELLTYPE_INT)
    {
        eDataType = GDT_Int32;
    }
    else
    {
        eDataType = GDT_Float32;
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr AIGRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);
    GInt32 *panGridRaster;

    if (poODS->psInfo->nCellType == AIG_CELLTYPE_INT)
    {
        panGridRaster = (GInt32 *)VSIMalloc3(4, nBlockXSize, nBlockYSize);
        if (panGridRaster == nullptr ||
            AIGReadTile(poODS->psInfo, nBlockXOff, nBlockYOff, panGridRaster) !=
                CE_None)
        {
            CPLFree(panGridRaster);
            return CE_Failure;
        }

        if (eDataType == GDT_Byte)
        {
            for (int i = 0; i < nBlockXSize * nBlockYSize; i++)
            {
                if (panGridRaster[i] == ESRI_GRID_NO_DATA)
                    ((GByte *)pImage)[i] = 255;
                else
                    ((GByte *)pImage)[i] = (GByte)panGridRaster[i];
            }
        }
        else if (eDataType == GDT_Int16)
        {
            for (int i = 0; i < nBlockXSize * nBlockYSize; i++)
            {
                if (panGridRaster[i] == ESRI_GRID_NO_DATA)
                    ((GInt16 *)pImage)[i] = -32768;
                else
                    ((GInt16 *)pImage)[i] = (GInt16)panGridRaster[i];
            }
        }
        else
        {
            for (int i = 0; i < nBlockXSize * nBlockYSize; i++)
                ((GInt32 *)pImage)[i] = panGridRaster[i];
        }

        CPLFree(panGridRaster);

        return CE_None;
    }
    else
    {
        return AIGReadFloatTile(poODS->psInfo, nBlockXOff, nBlockYOff,
                                (float *)pImage);
    }
}

/************************************************************************/
/*                           GetDefaultRAT()                            */
/************************************************************************/

GDALRasterAttributeTable *AIGRasterBand::GetDefaultRAT()

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);

    /* -------------------------------------------------------------------- */
    /*      Read info raster attribute table, if present.                   */
    /* -------------------------------------------------------------------- */
    if (!poODS->bHasReadRat)
    {
        poODS->ReadRAT();
        poODS->bHasReadRat = true;
    }

    if (poODS->poRAT)
        return poODS->poRAT;
    else
        return GDALPamRasterBand::GetDefaultRAT();
}

/************************************************************************/
/*                             GetMinimum()                             */
/************************************************************************/

double AIGRasterBand::GetMinimum(int *pbSuccess)

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);

    if (pbSuccess != nullptr)
        *pbSuccess = TRUE;

    return poODS->psInfo->dfMin;
}

/************************************************************************/
/*                             GetMaximum()                             */
/************************************************************************/

double AIGRasterBand::GetMaximum(int *pbSuccess)

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);

    if (pbSuccess != nullptr)
        *pbSuccess = TRUE;

    return poODS->psInfo->dfMax;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double AIGRasterBand::GetNoDataValue(int *pbSuccess)

{
    if (pbSuccess != nullptr)
        *pbSuccess = TRUE;

    if (eDataType == GDT_Float32)
        return ESRI_GRID_FLOAT_NO_DATA;

    if (eDataType == GDT_Int16)
        return -32768;

    if (eDataType == GDT_Byte)
        return 255;

    return ESRI_GRID_NO_DATA;
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp AIGRasterBand::GetColorInterpretation()

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);

    if (poODS->poCT != nullptr)
        return GCI_PaletteIndex;

    return GDALPamRasterBand::GetColorInterpretation();
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *AIGRasterBand::GetColorTable()

{
    AIGDataset *poODS = cpl::down_cast<AIGDataset *>(poDS);

    if (poODS->poCT != nullptr)
        return poODS->poCT;

    return GDALPamRasterBand::GetColorTable();
}

/************************************************************************/
/* ==================================================================== */
/*                            AIGDataset                               */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            AIGDataset()                            */
/************************************************************************/

AIGDataset::AIGDataset()
    : psInfo(nullptr), papszPrj(nullptr), poCT(nullptr), bHasReadRat(false),
      poRAT(nullptr)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                           ~AIGDataset()                            */
/************************************************************************/

AIGDataset::~AIGDataset()

{
    FlushCache(true);
    CSLDestroy(papszPrj);
    if (psInfo != nullptr)
        AIGClose(psInfo);

    if (poCT != nullptr)
        delete poCT;

    if (poRAT != nullptr)
        delete poRAT;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **AIGDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    // Add in all files in the cover directory.
    char **papszCoverFiles = VSIReadDir(GetDescription());

    for (int i = 0; papszCoverFiles != nullptr && papszCoverFiles[i] != nullptr;
         i++)
    {
        if (EQUAL(papszCoverFiles[i], ".") || EQUAL(papszCoverFiles[i], ".."))
            continue;

        papszFileList = CSLAddString(
            papszFileList,
            CPLFormFilenameSafe(GetDescription(), papszCoverFiles[i], nullptr)
                .c_str());
    }
    CSLDestroy(papszCoverFiles);

    return papszFileList;
}

/************************************************************************/
/*                          AIGErrorHandlerVATOpen()                    */
/************************************************************************/

class AIGErrorDescription
{
  public:
    CPLErr eErr;
    CPLErrorNum no;
    std::string osMsg;
};

static void CPL_STDCALL AIGErrorHandlerVATOpen(CPLErr eErr, CPLErrorNum no,
                                               const char *msg)
{
    std::vector<AIGErrorDescription> *paoErrors =
        (std::vector<AIGErrorDescription> *)CPLGetErrorHandlerUserData();
    if (STARTS_WITH_CI(msg, "EOF encountered in") &&
        strstr(msg, "../info/arc.dir") != nullptr)
        return;
    if (STARTS_WITH_CI(msg, "Failed to open table "))
        return;
    AIGErrorDescription oError;
    oError.eErr = eErr;
    oError.no = no;
    oError.osMsg = msg;
    paoErrors->push_back(std::move(oError));
}

/************************************************************************/
/*                              ReadRAT()                               */
/************************************************************************/

void AIGDataset::ReadRAT()

{
    /* -------------------------------------------------------------------- */
    /*      Check if we have an associated info directory.  If not          */
    /*      return quietly.                                                 */
    /* -------------------------------------------------------------------- */
    CPLString osInfoPath, osTableName;
    VSIStatBufL sStatBuf;

    osInfoPath = psInfo->pszCoverName;
    osInfoPath += "/../info";

    if (VSIStatL(osInfoPath, &sStatBuf) != 0)
    {
        CPLDebug("AIG", "No associated info directory at: %s, skip RAT.",
                 osInfoPath.c_str());
        return;
    }

    osInfoPath += "/";

    /* -------------------------------------------------------------------- */
    /*      Attempt to open the VAT table associated with this coverage.    */
    /* -------------------------------------------------------------------- */
    osTableName = CPLGetFilename(psInfo->pszCoverName);
    osTableName += ".VAT";

    /* Turn off errors that can be triggered if the info has no VAT */
    /* table related with this coverage */
    std::vector<AIGErrorDescription> aoErrors;
    CPLPushErrorHandlerEx(AIGErrorHandlerVATOpen, &aoErrors);

    AVCBinFile *psFile = AVCBinReadOpen(
        osInfoPath, osTableName, AVCCoverTypeUnknown, AVCFileTABLE, nullptr);
    CPLPopErrorHandler();

    /* Emit other errors */
    std::vector<AIGErrorDescription>::const_iterator oIter;
    for (oIter = aoErrors.begin(); oIter != aoErrors.end(); ++oIter)
    {
        const AIGErrorDescription &oError = *oIter;
        CPLError(oError.eErr, oError.no, "%s", oError.osMsg.c_str());
    }

    CPLErrorReset();
    if (psFile == nullptr)
        return;

    AVCTableDef *psTableDef = psFile->hdr.psTableDef;

    /* -------------------------------------------------------------------- */
    /*      Setup columns in corresponding RAT.                             */
    /* -------------------------------------------------------------------- */
    poRAT = new GDALDefaultRasterAttributeTable();

    for (int iField = 0; iField < psTableDef->numFields; iField++)
    {
        AVCFieldInfo *psFDef = psTableDef->pasFieldDef + iField;
        GDALRATFieldUsage eFUsage = GFU_Generic;
        GDALRATFieldType eFType = GFT_String;

        CPLString osFName = psFDef->szName;
        osFName.Trim();

        if (EQUAL(osFName, "VALUE"))
            eFUsage = GFU_MinMax;
        else if (EQUAL(osFName, "COUNT"))
            eFUsage = GFU_PixelCount;

        if (psFDef->nType1 * 10 == AVC_FT_BININT)
            eFType = GFT_Integer;
        else if (psFDef->nType1 * 10 == AVC_FT_BINFLOAT)
            eFType = GFT_Real;

        poRAT->CreateColumn(osFName, eFType, eFUsage);
    }

    /* -------------------------------------------------------------------- */
    /*      Process all records into RAT.                                   */
    /* -------------------------------------------------------------------- */
    AVCField *pasFields = nullptr;
    int iRecord = 0;

    while ((pasFields = AVCBinReadNextTableRec(psFile)) != nullptr)
    {
        iRecord++;

        for (int iField = 0; iField < psTableDef->numFields; iField++)
        {
            switch (psTableDef->pasFieldDef[iField].nType1 * 10)
            {
                case AVC_FT_DATE:
                case AVC_FT_FIXINT:
                case AVC_FT_CHAR:
                case AVC_FT_FIXNUM:
                {
                    // XXX - I bet mloskot would like to see const_cast +
                    // static_cast :-)
                    const char *pszTmp =
                        (const char *)(pasFields[iField].pszStr);
                    CPLString osStrValue(pszTmp);
                    poRAT->SetValue(iRecord - 1, iField, osStrValue.Trim());
                }
                break;

                case AVC_FT_BININT:
                    if (psTableDef->pasFieldDef[iField].nSize == 4)
                        poRAT->SetValue(iRecord - 1, iField,
                                        pasFields[iField].nInt32);
                    else
                        poRAT->SetValue(iRecord - 1, iField,
                                        pasFields[iField].nInt16);
                    break;

                case AVC_FT_BINFLOAT:
                    if (psTableDef->pasFieldDef[iField].nSize == 4)
                        poRAT->SetValue(iRecord - 1, iField,
                                        pasFields[iField].fFloat);
                    else
                        poRAT->SetValue(iRecord - 1, iField,
                                        pasFields[iField].dDouble);
                    break;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */

    AVCBinReadClose(psFile);

    /* Workaround against #2447 and #3031, to avoid binding languages */
    /* not being able to open the dataset */
    CPLErrorReset();
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *AIGDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      If the pass name ends in .adf assume a file within the          */
    /*      coverage has been selected, and strip that off the coverage     */
    /*      name.                                                           */
    /* -------------------------------------------------------------------- */
    CPLString osCoverName;

    osCoverName = poOpenInfo->pszFilename;
    if (osCoverName.size() > 4 &&
        EQUAL(osCoverName.c_str() + osCoverName.size() - 4, ".adf"))
    {
        osCoverName = CPLGetDirnameSafe(poOpenInfo->pszFilename);
        if (osCoverName == "")
            osCoverName = ".";
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise verify we were already given a directory.             */
    /* -------------------------------------------------------------------- */
    else if (!poOpenInfo->bIsDirectory)
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Verify that a few of the "standard" files are available.        */
    /* -------------------------------------------------------------------- */
    VSIStatBufL sStatBuf;
    CPLString osTestName;

    osTestName.Printf("%s/hdr.adf", osCoverName.c_str());
    if (VSIStatL(osTestName, &sStatBuf) != 0)
    {
        osTestName.Printf("%s/HDR.ADF", osCoverName.c_str());
        if (VSIStatL(osTestName, &sStatBuf) != 0)
            return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm we have at least one raster data file.  These can be    */
    /*      sparse so we don't require particular ones to exists but if     */
    /*      there are none this is likely not a grid.                       */
    /* -------------------------------------------------------------------- */
    char **papszFileList = VSIReadDir(osCoverName);
    bool bGotOne = false;

    if (papszFileList == nullptr)
    {
        /* Useful when reading from /vsicurl/ on servers that don't */
        /* return a file list */
        /* such as
         * /vsicurl/http://eros.usgs.gov/archive/nslrsda/GeoTowns/NLCD/89110458
         */
        do
        {
            osTestName.Printf("%s/W001001.ADF", osCoverName.c_str());
            if (VSIStatL(osTestName, &sStatBuf) == 0)
            {
                bGotOne = true;
                break;
            }

            osTestName.Printf("%s/w001001.adf", osCoverName.c_str());
            if (VSIStatL(osTestName, &sStatBuf) == 0)
            {
                bGotOne = true;
                break;
            }
        } while (false);
    }

    for (int iFile = 0; papszFileList != nullptr &&
                        papszFileList[iFile] != nullptr && !bGotOne;
         iFile++)
    {
        if (strlen(papszFileList[iFile]) != 11)
            continue;

        // looking for something like w001001.adf or z001013.adf
        if (papszFileList[iFile][0] != 'w' && papszFileList[iFile][0] != 'W' &&
            papszFileList[iFile][0] != 'z' && papszFileList[iFile][0] != 'Z')
            continue;

        if (!STARTS_WITH(papszFileList[iFile] + 1, "0010"))
            continue;

        if (!EQUAL(papszFileList[iFile] + 7, ".adf"))
            continue;

        bGotOne = true;
    }
    CSLDestroy(papszFileList);

    if (!bGotOne)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Open the file.                                                  */
    /* -------------------------------------------------------------------- */
    AIGInfo_t *psInfo = AIGOpen(osCoverName.c_str(), "r");

    if (psInfo == nullptr)
    {
        CPLErrorReset();
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        AIGClose(psInfo);
        ReportUpdateNotSupportedByDriver("AIG");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    AIGDataset *poDS = new AIGDataset();

    poDS->psInfo = psInfo;

    /* -------------------------------------------------------------------- */
    /*      Try to read a color table (.clr).  It seems it is legal to      */
    /*      have more than one so we just use the first one found.          */
    /* -------------------------------------------------------------------- */
    char **papszFiles = VSIReadDir(psInfo->pszCoverName);
    CPLString osClrFilename;
    CPLString osCleanPath = CPLCleanTrailingSlashSafe(psInfo->pszCoverName);

    // first check for any .clr in coverage dir.
    for (int iFile = 0; papszFiles != nullptr && papszFiles[iFile] != nullptr;
         iFile++)
    {
        const std::string osExt = CPLGetExtensionSafe(papszFiles[iFile]);
        if (!EQUAL(osExt.c_str(), "clr") && !EQUAL(osExt.c_str(), "CLR"))
            continue;

        osClrFilename = CPLFormFilenameSafe(psInfo->pszCoverName,
                                            papszFiles[iFile], nullptr);
        break;
    }

    CSLDestroy(papszFiles);

    // Look in parent if we don't find a .clr in the coverage dir.
    if (osClrFilename.empty())
    {
        CPLString osTestClrFilename;
        osTestClrFilename.Printf("%s/../%s.clr", psInfo->pszCoverName,
                                 CPLGetFilename(osCleanPath));

        if (VSIStatL(osTestClrFilename, &sStatBuf) != 0)
        {
            osTestClrFilename.Printf("%s/../%s.CLR", psInfo->pszCoverName,
                                     CPLGetFilename(osCleanPath));

            if (!VSIStatL(osTestClrFilename, &sStatBuf))
                osClrFilename = std::move(osTestClrFilename);
        }
        else
            osClrFilename = std::move(osTestClrFilename);
    }

    if (!osClrFilename.empty())
        poDS->TranslateColorTable(osClrFilename);

    /* -------------------------------------------------------------------- */
    /*      Establish raster info.                                          */
    /* -------------------------------------------------------------------- */
    poDS->nRasterXSize = psInfo->nPixels;
    poDS->nRasterYSize = psInfo->nLines;
    poDS->nBands = 1;

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    poDS->SetBand(1, new AIGRasterBand(poDS, 1));

    /* -------------------------------------------------------------------- */
    /*      Try to read projection file.                                    */
    /* -------------------------------------------------------------------- */
    const std::string osPrjFilename =
        CPLFormCIFilenameSafe(psInfo->pszCoverName, "prj", "adf");
    if (VSIStatL(osPrjFilename.c_str(), &sStatBuf) == 0)
    {
        OGRSpatialReference oSRS;
        oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        poDS->papszPrj = CSLLoad(osPrjFilename.c_str());

        if (oSRS.importFromESRI(poDS->papszPrj) == OGRERR_NONE)
        {
            // If geographic values are in seconds, we must transform.
            // Is there a code for minutes too?
            if (oSRS.IsGeographic() &&
                EQUAL(OSR_GDS(poDS->papszPrj, "Units", ""), "DS"))
            {
                psInfo->dfLLX /= 3600.0;
                psInfo->dfURY /= 3600.0;
                psInfo->dfCellSizeX /= 3600.0;
                psInfo->dfCellSizeY /= 3600.0;
            }

            poDS->m_oSRS = std::move(oSRS);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(psInfo->pszCoverName);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Open overviews.                                                 */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, psInfo->pszCoverName);

    return poDS;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr AIGDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt[0] = psInfo->dfLLX;
    gt[1] = psInfo->dfCellSizeX;
    gt[2] = 0;

    gt[3] = psInfo->dfURY;
    gt[4] = 0;
    gt[5] = -psInfo->dfCellSizeY;

    return CE_None;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *AIGDataset::GetSpatialRef() const

{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                        TranslateColorTable()                         */
/************************************************************************/

void AIGDataset::TranslateColorTable(const char *pszClrFilename)

{
    char **papszClrLines = CSLLoad(pszClrFilename);
    if (papszClrLines == nullptr)
        return;

    poCT = new GDALColorTable();

    for (int iLine = 0; papszClrLines[iLine] != nullptr; iLine++)
    {
        char **papszTokens = CSLTokenizeString(papszClrLines[iLine]);

        if (CSLCount(papszTokens) >= 4 && papszTokens[0][0] != '#')
        {
            int nIndex;
            GDALColorEntry sEntry;

            nIndex = atoi(papszTokens[0]);
            sEntry.c1 = (short)atoi(papszTokens[1]);
            sEntry.c2 = (short)atoi(papszTokens[2]);
            sEntry.c3 = (short)atoi(papszTokens[3]);
            sEntry.c4 = 255;

            if ((nIndex < 0 || nIndex > 33000) ||
                (sEntry.c1 < 0 || sEntry.c1 > 255) ||
                (sEntry.c2 < 0 || sEntry.c2 > 255) ||
                (sEntry.c3 < 0 || sEntry.c3 > 255))
            {
                CSLDestroy(papszTokens);
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Color table entry appears to be corrupt, skipping "
                         "the rest. ");
                break;
            }

            poCT->SetColorEntry(nIndex, &sEntry);
        }

        CSLDestroy(papszTokens);
    }

    CSLDestroy(papszClrLines);
}

/************************************************************************/
/*                              OSR_GDS()                               */
/************************************************************************/

static CPLString OSR_GDS(char **papszNV, const char *pszField,
                         const char *pszDefaultValue)

{
    if (papszNV == nullptr || papszNV[0] == nullptr)
        return pszDefaultValue;

    int iLine = 0;
    for (; papszNV[iLine] != nullptr &&
           !EQUALN(papszNV[iLine], pszField, strlen(pszField));
         iLine++)
    {
    }

    if (papszNV[iLine] == nullptr)
        return pszDefaultValue;
    else
    {
        CPLString osResult;
        char **papszTokens = CSLTokenizeString(papszNV[iLine]);

        if (CSLCount(papszTokens) > 1)
            osResult = papszTokens[1];
        else
            osResult = pszDefaultValue;

        CSLDestroy(papszTokens);
        return osResult;
    }
}

/************************************************************************/
/*                             AIGRename()                              */
/*                                                                      */
/*      Custom renamer for AIG dataset.                                 */
/************************************************************************/

static CPLErr AIGRename(const char *pszNewName, const char *pszOldName)

{
    /* -------------------------------------------------------------------- */
    /*      Make sure we are talking about paths to the coverage            */
    /*      directory.                                                      */
    /* -------------------------------------------------------------------- */
    CPLString osOldPath, osNewPath;

    if (!CPLGetExtensionSafe(pszNewName).empty())
        osNewPath = CPLGetPathSafe(pszNewName);
    else
        osNewPath = pszNewName;

    if (!CPLGetExtensionSafe(pszOldName).empty())
        osOldPath = CPLGetPathSafe(pszOldName);
    else
        osOldPath = pszOldName;

    /* -------------------------------------------------------------------- */
    /*      Get file list.                                                  */
    /* -------------------------------------------------------------------- */

    GDALDatasetH hDS = GDALOpen(osOldPath, GA_ReadOnly);
    if (hDS == nullptr)
        return CE_Failure;

    char **papszFileList = GDALGetFileList(hDS);
    GDALClose(hDS);

    if (papszFileList == nullptr)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Work out the corresponding new names.                           */
    /* -------------------------------------------------------------------- */
    char **papszNewFileList = nullptr;

    for (int i = 0; papszFileList[i] != nullptr; i++)
    {
        CPLString osNewFilename;

        if (!EQUALN(papszFileList[i], osOldPath, osOldPath.size()))
        {
            CPLAssert(false);
            return CE_Failure;
        }

        osNewFilename = osNewPath + (papszFileList[i] + osOldPath.size());

        papszNewFileList = CSLAddString(papszNewFileList, osNewFilename);
    }

    /* -------------------------------------------------------------------- */
    /*      Try renaming the directory.                                     */
    /* -------------------------------------------------------------------- */
    if (VSIRename(osNewPath, osOldPath) != 0)
    {
        if (VSIMkdir(osNewPath, 0777) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create directory %s:\n%s", osNewPath.c_str(),
                     VSIStrerror(errno));
            CSLDestroy(papszNewFileList);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Copy/rename any remaining files.                                */
    /* -------------------------------------------------------------------- */
    VSIStatBufL sStatBuf;

    for (int i = 0; papszFileList[i] != nullptr; i++)
    {
        if (VSIStatL(papszFileList[i], &sStatBuf) == 0 &&
            VSI_ISREG(sStatBuf.st_mode))
        {
            if (CPLMoveFile(papszNewFileList[i], papszFileList[i]) != 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to move %s to %s:\n%s", papszFileList[i],
                         papszNewFileList[i], VSIStrerror(errno));
                CSLDestroy(papszNewFileList);
                return CE_Failure;
            }
        }
    }

    if (VSIStatL(osOldPath, &sStatBuf) == 0)
    {
        if (CPLUnlinkTree(osOldPath) != 0)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Unable to cleanup old path.");
        }
    }

    CSLDestroy(papszFileList);
    CSLDestroy(papszNewFileList);
    return CE_None;
}

/************************************************************************/
/*                             AIGDelete()                              */
/*                                                                      */
/*      Custom dataset deleter for AIG dataset.                         */
/************************************************************************/

static CPLErr AIGDelete(const char *pszDatasetname)

{
    /* -------------------------------------------------------------------- */
    /*      Get file list.                                                  */
    /* -------------------------------------------------------------------- */
    GDALDatasetH hDS = GDALOpen(pszDatasetname, GA_ReadOnly);
    if (hDS == nullptr)
        return CE_Failure;

    char **papszFileList = GDALGetFileList(hDS);
    GDALClose(hDS);

    if (papszFileList == nullptr)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Delete all regular files.                                       */
    /* -------------------------------------------------------------------- */
    for (int i = 0; papszFileList[i] != nullptr; i++)
    {
        VSIStatBufL sStatBuf;
        if (VSIStatL(papszFileList[i], &sStatBuf) == 0 &&
            VSI_ISREG(sStatBuf.st_mode))
        {
            if (VSIUnlink(papszFileList[i]) != 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to delete '%s':\n%s", papszFileList[i],
                         VSIStrerror(errno));
                return CE_Failure;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Delete directories.                                             */
    /* -------------------------------------------------------------------- */
    for (int i = 0; papszFileList[i] != nullptr; i++)
    {
        VSIStatBufL sStatBuf;
        if (VSIStatL(papszFileList[i], &sStatBuf) == 0 &&
            VSI_ISDIR(sStatBuf.st_mode))
        {
            if (CPLUnlinkTree(papszFileList[i]) != 0)
                return CE_Failure;
        }
    }

    return CE_None;
}

/************************************************************************/
/*                          GDALRegister_AIG()                          */
/************************************************************************/

void GDALRegister_AIGrid()

{
    if (GDALGetDriverByName("AIG") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("AIG");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Arc/Info Binary Grid");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/aig.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = AIGDataset::Open;

    poDriver->pfnRename = AIGRename;
    poDriver->pfnDelete = AIGDelete;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
