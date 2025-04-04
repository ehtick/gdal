/**********************************************************************
 *
 * Name:     mitab_ogr_drive.h
 * Project:  Mid/mif tab ogr support
 * Language: C++
 * Purpose:  Header file containing public definitions for the library.
 * Author:   Stephane Villeneuve, stephane.v@videotron.ca
 *
 **********************************************************************
 * Copyright (c) 1999, 2000, Stephane Villeneuve
 * Copyright (c) 2014, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 **********************************************************************/

#include "mitab.h"
#include "ogrsf_frmts.h"

#ifndef MITAB_OGR_DRIVER_H_INCLUDED_
#define MITAB_OGR_DRIVER_H_INCLUDED_

/*=====================================================================
 *            OGRTABDataSource Class
 *
 * These classes handle all the file types supported by the MITAB lib.
 * through the IMapInfoFile interface.
 *====================================================================*/
class OGRTABDataSource : public GDALDataset
{
    CPL_DISALLOW_COPY_ASSIGN(OGRTABDataSource)

  private:
    char *m_pszDirectory;

    int m_nLayerCount;
    IMapInfoFile **m_papoLayers;

    char **m_papszOptions;
    int m_bCreateMIF;
    int m_bSingleFile;
    int m_bSingleLayerAlreadyCreated;
    GBool m_bQuickSpatialIndexMode;
    int m_nBlockSize;

  private:
    inline bool GetUpdate() const
    {
        return eAccess == GA_Update;
    }

  public:
    OGRTABDataSource();
    virtual ~OGRTABDataSource();

    int Open(GDALOpenInfo *poOpenInfo, int bTestOpen);
    int Create(const char *pszName, char **papszOptions);

    int GetLayerCount() override;
    OGRLayer *GetLayer(int) override;
    int TestCapability(const char *) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;

    char **GetFileList() override;

    virtual OGRLayer *ExecuteSQL(const char *pszStatement,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
};

void CPL_DLL RegisterOGRTAB();

#endif /* MITAB_OGR_DRIVER_H_INCLUDED_ */
