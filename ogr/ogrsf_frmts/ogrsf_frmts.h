/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Classes related to format registration, and file opening.
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGRSF_FRMTS_H_INCLUDED
#define OGRSF_FRMTS_H_INCLUDED

#include "cpl_progress.h"
#include "ogr_feature.h"
#include "ogr_featurestyle.h"
#include "gdal_priv.h"

#include <memory>
#include <deque>

/**
 * \file ogrsf_frmts.h
 *
 * Classes related to registration of format support, and opening datasets.
 */

//! @cond Doxygen_Suppress
#if !defined(GDAL_COMPILATION) && !defined(SUPPRESS_DEPRECATION_WARNINGS)
#define OGR_DEPRECATED(x) CPL_WARN_DEPRECATED(x)
#else
#define OGR_DEPRECATED(x)
#endif

#ifndef CPPCHECK_STATIC
#define CPPCHECK_STATIC
#endif
//! @endcond

class OGRLayerAttrIndex;
class OGRSFDriver;

struct ArrowArrayStream;

/************************************************************************/
/*                               OGRLayer                               */
/************************************************************************/

/**
 * This class represents a layer of simple features, with access methods.
 *
 */

/* Note: any virtual method added to this class must also be added in the */
/* OGRLayerDecorator and OGRMutexedLayer classes. */

class CPL_DLL OGRLayer : public GDALMajorObject
{
  private:
    struct Private;
    std::unique_ptr<Private> m_poPrivate;

    void ConvertGeomsIfNecessary(OGRFeature *poFeature);

    class CPL_DLL FeatureIterator
    {
        struct Private;
        std::unique_ptr<Private> m_poPrivate;

      public:
        FeatureIterator(OGRLayer *poLayer, bool bStart);
        FeatureIterator(
            FeatureIterator &&oOther) noexcept;  // declared but not defined.
                                                 // Needed for gcc 5.4 at least
        ~FeatureIterator();
        OGRFeatureUniquePtr &operator*();
        FeatureIterator &operator++();
        bool operator!=(const FeatureIterator &it) const;
    };

    friend inline FeatureIterator begin(OGRLayer *poLayer);
    friend inline FeatureIterator end(OGRLayer *poLayer);

    CPL_DISALLOW_COPY_ASSIGN(OGRLayer)

  protected:
    //! @cond Doxygen_Suppress
    int m_bFilterIsEnvelope;
    OGRGeometry *m_poFilterGeom;
    OGRPreparedGeometry *m_pPreparedFilterGeom; /* m_poFilterGeom compiled as a
                                                   prepared geometry */
    OGREnvelope m_sFilterEnvelope;
    int m_iGeomFieldFilter;  // specify the index on which the spatial
                             // filter is active.

    int FilterGeometry(const OGRGeometry *);
    // int          FilterGeometry( OGRGeometry *, OGREnvelope*
    // psGeometryEnvelope);
    int InstallFilter(const OGRGeometry *);
    bool
    ValidateGeometryFieldIndexForSetSpatialFilter(int iGeomField,
                                                  const OGRGeometry *poGeomIn,
                                                  bool bIsSelectLayer = false);
    //! @endcond

    virtual OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                              bool bForce) CPL_WARN_UNUSED_RESULT;

    virtual OGRErr IGetExtent3D(int iGeomField, OGREnvelope3D *psExtent3D,
                                bool bForce) CPL_WARN_UNUSED_RESULT;

    virtual OGRErr ISetSpatialFilter(int iGeomField, const OGRGeometry *);

    virtual OGRErr ISetFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    virtual OGRErr ICreateFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    virtual OGRErr IUpsertFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    virtual OGRErr
    IUpdateFeature(OGRFeature *poFeature, int nUpdatedFieldsCount,
                   const int *panUpdatedFieldsIdx, int nUpdatedGeomFieldsCount,
                   const int *panUpdatedGeomFieldsIdx,
                   bool bUpdateStyleString) CPL_WARN_UNUSED_RESULT;

    //! @cond Doxygen_Suppress
    CPLStringList m_aosArrowArrayStreamOptions{};

    friend struct OGRGenSQLResultsLayerArrowStreamPrivateData;

    struct ArrowArrayStreamPrivateData
    {
        bool m_bArrowArrayStreamInProgress = false;
        bool m_bEOF = false;
        OGRLayer *m_poLayer = nullptr;
        std::vector<GIntBig> m_anQueriedFIDs{};
        size_t m_iQueriedFIDS = 0;
        std::deque<std::unique_ptr<OGRFeature>> m_oFeatureQueue{};
    };

    std::shared_ptr<ArrowArrayStreamPrivateData>
        m_poSharedArrowArrayStreamPrivateData{};

    struct ArrowArrayStreamPrivateDataSharedDataWrapper
    {
        std::shared_ptr<ArrowArrayStreamPrivateData> poShared{};
    };
    //! @endcond

    friend class OGRArrowArrayHelper;
    friend class OGRGenSQLResultsLayer;
    static void ReleaseArray(struct ArrowArray *array);
    static void ReleaseSchema(struct ArrowSchema *schema);
    static void ReleaseStream(struct ArrowArrayStream *stream);
    virtual int GetArrowSchema(struct ArrowArrayStream *,
                               struct ArrowSchema *out_schema);
    virtual int GetNextArrowArray(struct ArrowArrayStream *,
                                  struct ArrowArray *out_array);
    static int StaticGetArrowSchema(struct ArrowArrayStream *,
                                    struct ArrowSchema *out_schema);
    static int StaticGetNextArrowArray(struct ArrowArrayStream *,
                                       struct ArrowArray *out_array);
    static const char *GetLastErrorArrowArrayStream(struct ArrowArrayStream *);

    static struct ArrowSchema *
    CreateSchemaForWKBGeometryColumn(const OGRGeomFieldDefn *poFieldDefn,
                                     const char *pszArrowFormat,
                                     const char *pszExtensionName);

    virtual bool
    CanPostFilterArrowArray(const struct ArrowSchema *schema) const;
    void PostFilterArrowArray(const struct ArrowSchema *schema,
                              struct ArrowArray *array,
                              CSLConstList papszOptions) const;

    //! @cond Doxygen_Suppress
    bool CreateFieldFromArrowSchemaInternal(const struct ArrowSchema *schema,
                                            const std::string &osFieldPrefix,
                                            CSLConstList papszOptions);
    //! @endcond

  public:
    OGRLayer();
    virtual ~OGRLayer();

    /** Return begin of feature iterator.
     *
     * Using this iterator for standard range-based loops is safe, but
     * due to implementation limitations, you shouldn't try to access
     * (dereference) more than one iterator step at a time, since the
     * OGRFeatureUniquePtr reference is reused.
     *
     * Only one iterator per layer can be active at a time.
     * @since GDAL 2.3
     */
    FeatureIterator begin();

    /** Return end of feature iterator. */
    FeatureIterator end();

    virtual OGRGeometry *GetSpatialFilter();

    OGRErr SetSpatialFilter(const OGRGeometry *);
    OGRErr SetSpatialFilterRect(double dfMinX, double dfMinY, double dfMaxX,
                                double dfMaxY);

    OGRErr SetSpatialFilter(int iGeomField, const OGRGeometry *);
    OGRErr SetSpatialFilterRect(int iGeomField, double dfMinX, double dfMinY,
                                double dfMaxX, double dfMaxY);

    virtual OGRErr SetAttributeFilter(const char *);

    virtual void ResetReading() = 0;
    virtual OGRFeature *GetNextFeature() CPL_WARN_UNUSED_RESULT = 0;
    virtual OGRErr SetNextByIndex(GIntBig nIndex);
    virtual OGRFeature *GetFeature(GIntBig nFID) CPL_WARN_UNUSED_RESULT;

    virtual GDALDataset *GetDataset();
    virtual bool GetArrowStream(struct ArrowArrayStream *out_stream,
                                CSLConstList papszOptions = nullptr);
    virtual bool IsArrowSchemaSupported(const struct ArrowSchema *schema,
                                        CSLConstList papszOptions,
                                        std::string &osErrorMsg) const;
    virtual bool
    CreateFieldFromArrowSchema(const struct ArrowSchema *schema,
                               CSLConstList papszOptions = nullptr);
    virtual bool WriteArrowBatch(const struct ArrowSchema *schema,
                                 struct ArrowArray *array,
                                 CSLConstList papszOptions = nullptr);

    OGRErr SetFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    OGRErr CreateFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    OGRErr UpsertFeature(OGRFeature *poFeature) CPL_WARN_UNUSED_RESULT;
    OGRErr UpdateFeature(OGRFeature *poFeature, int nUpdatedFieldsCount,
                         const int *panUpdatedFieldsIdx,
                         int nUpdatedGeomFieldsCount,
                         const int *panUpdatedGeomFieldsIdx,
                         bool bUpdateStyleString) CPL_WARN_UNUSED_RESULT;

    virtual OGRErr DeleteFeature(GIntBig nFID) CPL_WARN_UNUSED_RESULT;

    virtual const char *GetName();
    virtual OGRwkbGeometryType GetGeomType();
    virtual OGRFeatureDefn *GetLayerDefn() = 0;
    virtual int FindFieldIndex(const char *pszFieldName, int bExactMatch);

    virtual OGRSpatialReference *GetSpatialRef();

    /** Return type of OGRLayer::GetSupportedSRSList() */
    typedef std::vector<
        std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>>
        GetSupportedSRSListRetType;
    virtual const GetSupportedSRSListRetType &
    GetSupportedSRSList(int iGeomField);
    virtual OGRErr SetActiveSRS(int iGeomField,
                                const OGRSpatialReference *poSRS);

    virtual GIntBig GetFeatureCount(int bForce = TRUE);

    OGRErr GetExtent(OGREnvelope *psExtent,
                     bool bForce = true) CPL_WARN_UNUSED_RESULT;
    OGRErr GetExtent(int iGeomField, OGREnvelope *psExtent,
                     bool bForce = true) CPL_WARN_UNUSED_RESULT;

    OGRErr GetExtent3D(int iGeomField, OGREnvelope3D *psExtent,
                       bool bForce = true) CPL_WARN_UNUSED_RESULT;

    virtual int TestCapability(const char *) = 0;

    virtual OGRErr Rename(const char *pszNewName) CPL_WARN_UNUSED_RESULT;

    virtual OGRErr CreateField(const OGRFieldDefn *poField,
                               int bApproxOK = TRUE);
    virtual OGRErr DeleteField(int iField);
    virtual OGRErr ReorderFields(int *panMap);
    virtual OGRErr AlterFieldDefn(int iField, OGRFieldDefn *poNewFieldDefn,
                                  int nFlagsIn);
    virtual OGRErr
    AlterGeomFieldDefn(int iGeomField,
                       const OGRGeomFieldDefn *poNewGeomFieldDefn,
                       int nFlagsIn);

    virtual OGRErr CreateGeomField(const OGRGeomFieldDefn *poField,
                                   int bApproxOK = TRUE);

    virtual OGRErr SyncToDisk();

    virtual OGRStyleTable *GetStyleTable();
    virtual void SetStyleTableDirectly(OGRStyleTable *poStyleTable);

    virtual void SetStyleTable(OGRStyleTable *poStyleTable);

    virtual OGRErr StartTransaction() CPL_WARN_UNUSED_RESULT;
    virtual OGRErr CommitTransaction() CPL_WARN_UNUSED_RESULT;
    virtual OGRErr RollbackTransaction();

    //! @cond Doxygen_Suppress
    // Keep field definitions in sync with transactions
    virtual void PrepareStartTransaction();
    // Rollback TO SAVEPOINT if osSavepointName is not empty, otherwise ROLLBACK
    virtual void FinishRollbackTransaction(const std::string &osSavepointName);
    //! @endcond

    virtual const char *GetFIDColumn();
    virtual const char *GetGeometryColumn();

    virtual OGRErr SetIgnoredFields(CSLConstList papszFields);

    virtual OGRGeometryTypeCounter *
    GetGeometryTypes(int iGeomField, int nFlagsGGT, int &nEntryCountOut,
                     GDALProgressFunc pfnProgress, void *pProgressData);

    OGRErr Intersection(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                        char **papszOptions = nullptr,
                        GDALProgressFunc pfnProgress = nullptr,
                        void *pProgressArg = nullptr);
    OGRErr Union(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                 char **papszOptions = nullptr,
                 GDALProgressFunc pfnProgress = nullptr,
                 void *pProgressArg = nullptr);
    OGRErr SymDifference(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                         char **papszOptions, GDALProgressFunc pfnProgress,
                         void *pProgressArg);
    OGRErr Identity(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                    char **papszOptions = nullptr,
                    GDALProgressFunc pfnProgress = nullptr,
                    void *pProgressArg = nullptr);
    OGRErr Update(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                  char **papszOptions = nullptr,
                  GDALProgressFunc pfnProgress = nullptr,
                  void *pProgressArg = nullptr);
    OGRErr Clip(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                char **papszOptions = nullptr,
                GDALProgressFunc pfnProgress = nullptr,
                void *pProgressArg = nullptr);
    OGRErr Erase(OGRLayer *pLayerMethod, OGRLayer *pLayerResult,
                 char **papszOptions = nullptr,
                 GDALProgressFunc pfnProgress = nullptr,
                 void *pProgressArg = nullptr);

    int Reference();
    int Dereference();
    int GetRefCount() const;
    //! @cond Doxygen_Suppress
    GIntBig GetFeaturesRead();
    //! @endcond

    /* non virtual : convenience wrapper for ReorderFields() */
    OGRErr ReorderField(int iOldFieldPos, int iNewFieldPos);

    //! @cond Doxygen_Suppress
    int AttributeFilterEvaluationNeedsGeometry();

    /* consider these private */
    OGRErr InitializeIndexSupport(const char *);

    OGRLayerAttrIndex *GetIndex()
    {
        return m_poAttrIndex;
    }

    int GetGeomFieldFilter() const
    {
        return m_iGeomFieldFilter;
    }

    const char *GetAttrQueryString() const
    {
        return m_pszAttrQueryString;
    }

    //! @endcond

    /** Convert a OGRLayer* to a OGRLayerH.
     * @since GDAL 2.3
     */
    static inline OGRLayerH ToHandle(OGRLayer *poLayer)
    {
        return reinterpret_cast<OGRLayerH>(poLayer);
    }

    /** Convert a OGRLayerH to a OGRLayer*.
     * @since GDAL 2.3
     */
    static inline OGRLayer *FromHandle(OGRLayerH hLayer)
    {
        return reinterpret_cast<OGRLayer *>(hLayer);
    }

    //! @cond Doxygen_Suppress
    bool FilterWKBGeometry(const GByte *pabyWKB, size_t nWKBSize,
                           bool bEnvelopeAlreadySet,
                           OGREnvelope &sEnvelope) const;

    static bool FilterWKBGeometry(const GByte *pabyWKB, size_t nWKBSize,
                                  bool bEnvelopeAlreadySet,
                                  OGREnvelope &sEnvelope,
                                  const OGRGeometry *poFilterGeom,
                                  bool bFilterIsEnvelope,
                                  const OGREnvelope &sFilterEnvelope,
                                  OGRPreparedGeometry *&poPreparedFilterGeom);
    //! @endcond

    /** Field name used by GetArrowSchema() for a FID column when
     * GetFIDColumn() is not set.
     */
    static constexpr const char *DEFAULT_ARROW_FID_NAME = "OGC_FID";

    /** Field name used by GetArrowSchema() for the name of the (single)
     * geometry column (returned by GetGeometryColumn()) is not set.
     */
    static constexpr const char *DEFAULT_ARROW_GEOMETRY_NAME = "wkb_geometry";

  protected:
    //! @cond Doxygen_Suppress

    enum class FieldChangeType : char
    {
        ADD_FIELD,
        ALTER_FIELD,
        DELETE_FIELD
    };

    // Store changes to the fields that happened inside a transaction
    template <typename T> struct FieldDefnChange
    {

        FieldDefnChange(std::unique_ptr<T> &&poFieldDefnIn, int iFieldIn,
                        FieldChangeType eChangeTypeIn,
                        const std::string &osSavepointNameIn = "")
            : poFieldDefn(std::move(poFieldDefnIn)), iField(iFieldIn),
              eChangeType(eChangeTypeIn), osSavepointName(osSavepointNameIn)
        {
        }

        std::unique_ptr<T> poFieldDefn;
        int iField;
        FieldChangeType eChangeType;
        std::string osSavepointName;
    };

    std::vector<FieldDefnChange<OGRFieldDefn>> m_apoFieldDefnChanges{};
    std::vector<FieldDefnChange<OGRGeomFieldDefn>> m_apoGeomFieldDefnChanges{};

    OGRStyleTable *m_poStyleTable;
    OGRFeatureQuery *m_poAttrQuery;
    char *m_pszAttrQueryString;
    OGRLayerAttrIndex *m_poAttrIndex;

    int m_nRefCount;

    GIntBig m_nFeaturesRead;
    //! @endcond
};

/** Return begin of feature iterator.
 *
 * Using this iterator for standard range-based loops is safe, but
 * due to implementation limitations, you shouldn't try to access
 * (dereference) more than one iterator step at a time, since the
 * std::unique_ptr&lt;OGRFeature&gt; reference is reused.
 *
 * Only one iterator per layer can be active at a time.
 * @since GDAL 2.3
 * @see OGRLayer::begin()
 */
inline OGRLayer::FeatureIterator begin(OGRLayer *poLayer)
{
    return poLayer->begin();
}

/** Return end of feature iterator.
 * @see OGRLayer::end()
 */
inline OGRLayer::FeatureIterator end(OGRLayer *poLayer)
{
    return poLayer->end();
}

/** Unique pointer type for OGRLayer.
 * @since GDAL 3.2
 */
using OGRLayerUniquePtr = std::unique_ptr<OGRLayer>;

/************************************************************************/
/*                     OGRGetNextFeatureThroughRaw                      */
/************************************************************************/

/** Template class offering a GetNextFeature() implementation relying on
 * GetNextRawFeature()
 *
 * @since GDAL 3.2
 */
template <class BaseLayer> class OGRGetNextFeatureThroughRaw
{
  protected:
    ~OGRGetNextFeatureThroughRaw() = default;

  public:
    /** Implement OGRLayer::GetNextFeature(), relying on
     * BaseLayer::GetNextRawFeature() */
    OGRFeature *GetNextFeature()
    {
        const auto poThis = static_cast<BaseLayer *>(this);
        while (true)
        {
            OGRFeature *poFeature = poThis->GetNextRawFeature();
            if (poFeature == nullptr)
                return nullptr;

            if ((poThis->m_poFilterGeom == nullptr ||
                 poThis->FilterGeometry(poFeature->GetGeometryRef())) &&
                (poThis->m_poAttrQuery == nullptr ||
                 poThis->m_poAttrQuery->Evaluate(poFeature)))
            {
                return poFeature;
            }
            else
                delete poFeature;
        }
    }
};

/** Utility macro to define GetNextFeature() through GetNextRawFeature() */
#define DEFINE_GET_NEXT_FEATURE_THROUGH_RAW(BaseLayer)                         \
  private:                                                                     \
    friend class OGRGetNextFeatureThroughRaw<BaseLayer>;                       \
                                                                               \
  public:                                                                      \
    OGRFeature *GetNextFeature() override                                      \
    {                                                                          \
        return OGRGetNextFeatureThroughRaw<BaseLayer>::GetNextFeature();       \
    }

/************************************************************************/
/*                            OGRDataSource                             */
/************************************************************************/

/**
 * LEGACY class. Use GDALDataset in your new code ! This class may be
 * removed in a later release.
 *
 * This class represents a data source.  A data source potentially
 * consists of many layers (OGRLayer).  A data source normally consists
 * of one, or a related set of files, though the name doesn't have to be
 * a real item in the file system.
 *
 * When an OGRDataSource is destroyed, all its associated OGRLayers objects
 * are also destroyed.
 *
 * NOTE: Starting with GDAL 2.0, it is *NOT* safe to cast the handle of
 * a C function that returns a OGRDataSourceH to a OGRDataSource*. If a C++
 * object is needed, the handle should be cast to GDALDataset*.
 *
 * @deprecated
 */

class CPL_DLL OGRDataSource : public GDALDataset
{
  public:
    OGRDataSource();
    ~OGRDataSource() override;

    //! @cond Doxygen_Suppress
    virtual const char *GetName()
        OGR_DEPRECATED("Use GDALDataset class instead") = 0;

    static void DestroyDataSource(OGRDataSource *)
        OGR_DEPRECATED("Use GDALDataset class instead");
    //! @endcond
};

/************************************************************************/
/*                             OGRSFDriver                              */
/************************************************************************/

/**
 * LEGACY class. Use GDALDriver in your new code ! This class may be
 * removed in a later release.
 *
 * Represents an operational format driver.
 *
 * One OGRSFDriver derived class will normally exist for each file format
 * registered for use, regardless of whether a file has or will be opened.
 * The list of available drivers is normally managed by the
 * OGRSFDriverRegistrar.
 *
 * NOTE: Starting with GDAL 2.0, it is *NOT* safe to cast the handle of
 * a C function that returns a OGRSFDriverH to a OGRSFDriver*. If a C++ object
 * is needed, the handle should be cast to GDALDriver*.
 *
 * @deprecated
 */

class CPL_DLL OGRSFDriver : public GDALDriver
{
  public:
    //! @cond Doxygen_Suppress
    virtual ~OGRSFDriver();

    virtual const char *GetName()
        OGR_DEPRECATED("Use GDALDriver class instead") = 0;

    virtual OGRDataSource *Open(const char *pszName, int bUpdate = FALSE)
        OGR_DEPRECATED("Use GDALDriver class instead") = 0;

    virtual int TestCapability(const char *pszCap)
        OGR_DEPRECATED("Use GDALDriver class instead") = 0;

    virtual OGRDataSource *CreateDataSource(const char *pszName,
                                            char ** = nullptr)
        OGR_DEPRECATED("Use GDALDriver class instead");
    virtual OGRErr DeleteDataSource(const char *pszName)
        OGR_DEPRECATED("Use GDALDriver class instead");
    //! @endcond
};

/************************************************************************/
/*                         OGRSFDriverRegistrar                         */
/************************************************************************/

/**
 * LEGACY class. Use GDALDriverManager in your new code ! This class may be
 * removed in a later release.
 *
 * Singleton manager for OGRSFDriver instances that will be used to try
 * and open datasources.  Normally the registrar is populated with
 * standard drivers using the OGRRegisterAll() function and does not need
 * to be directly accessed.  The driver registrar and all registered drivers
 * may be cleaned up on shutdown using OGRCleanupAll().
 *
 * @deprecated
 */

class CPL_DLL OGRSFDriverRegistrar
{

    OGRSFDriverRegistrar();
    ~OGRSFDriverRegistrar();

    static GDALDataset *OpenWithDriverArg(GDALDriver *poDriver,
                                          GDALOpenInfo *poOpenInfo);
    static GDALDataset *CreateVectorOnly(GDALDriver *poDriver,
                                         const char *pszName,
                                         char **papszOptions);
    static CPLErr DeleteDataSource(GDALDriver *poDriver, const char *pszName);

  public:
    //! @cond Doxygen_Suppress
    static OGRSFDriverRegistrar *GetRegistrar()
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC void RegisterDriver(OGRSFDriver *poDriver)
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC int GetDriverCount(void)
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC GDALDriver *GetDriver(int iDriver)
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC GDALDriver *GetDriverByName(const char *)
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC int GetOpenDSCount()
        OGR_DEPRECATED("Use GDALDriverManager class instead");

    CPPCHECK_STATIC OGRDataSource *GetOpenDS(int)
        OGR_DEPRECATED("Use GDALDriverManager class instead");
    //! @endcond
};

/* -------------------------------------------------------------------- */
/*      Various available registration methods.                         */
/* -------------------------------------------------------------------- */
CPL_C_START

//! @cond Doxygen_Suppress
void OGRRegisterAllInternal();

void CPL_DLL RegisterOGRFileGDB();
void DeclareDeferredOGRFileGDBPlugin();
void CPL_DLL RegisterOGRShape();
void CPL_DLL RegisterOGRS57();
void CPL_DLL RegisterOGRTAB();
void CPL_DLL RegisterOGRMIF();
void CPL_DLL RegisterOGRODBC();
void DeclareDeferredOGRODBCPlugin();
void CPL_DLL RegisterOGRWAsP();
void CPL_DLL RegisterOGRPG();
void DeclareDeferredOGRPGPlugin();
void CPL_DLL RegisterOGRMSSQLSpatial();
void DeclareDeferredOGRMSSQLSpatialPlugin();
void CPL_DLL RegisterOGRMySQL();
void DeclareDeferredOGRMySQLPlugin();
void CPL_DLL RegisterOGROCI();
void DeclareDeferredOGROCIPlugin();
void CPL_DLL RegisterOGRDGN();
void CPL_DLL RegisterOGRGML();
void CPL_DLL RegisterOGRLIBKML();
void DeclareDeferredOGRLIBKMLPlugin();
void CPL_DLL RegisterOGRKML();
void CPL_DLL RegisterOGRFlatGeobuf();
void CPL_DLL RegisterOGRGeoJSON();
void CPL_DLL RegisterOGRGeoJSONSeq();
void CPL_DLL RegisterOGRESRIJSON();
void CPL_DLL RegisterOGRTopoJSON();
void CPL_DLL RegisterOGRAVCBin();
void CPL_DLL RegisterOGRAVCE00();
void CPL_DLL RegisterOGRVRT();
void CPL_DLL RegisterOGRSQLite();
void CPL_DLL RegisterOGRCSV();
void CPL_DLL RegisterOGRILI1();
void CPL_DLL RegisterOGRILI2();
void CPL_DLL RegisterOGRPGeo();
void CPL_DLL RegisterOGRDXF();
void CPL_DLL RegisterOGRCAD();
void DeclareDeferredOGRCADPlugin();
void CPL_DLL RegisterOGRDWG();
void CPL_DLL RegisterOGRDGNV8();
void DeclareDeferredOGRDWGPlugin();
void DeclareDeferredOGRDGNV8Plugin();
void CPL_DLL RegisterOGRIDB();
void DeclareDeferredOGRIDBPlugin();
void CPL_DLL RegisterOGRGMT();
void CPL_DLL RegisterOGRGPX();
void CPL_DLL RegisterOGRNAS();
void CPL_DLL RegisterOGRGeoRSS();
void CPL_DLL RegisterOGRVFK();
void DeclareDeferredOGRVFKPlugin();
void CPL_DLL RegisterOGRPGDump();
void CPL_DLL RegisterOGROSM();
void CPL_DLL RegisterOGRGPSBabel();
void CPL_DLL RegisterOGRPDS();
void CPL_DLL RegisterOGRWFS();
void CPL_DLL RegisterOGROAPIF();
void CPL_DLL RegisterOGRSOSI();
void DeclareDeferredOGRSOSIPlugin();
void CPL_DLL RegisterOGREDIGEO();
void CPL_DLL RegisterOGRIdrisi();
void CPL_DLL RegisterOGRXLS();
void DeclareDeferredOGRXLSPlugin();
void CPL_DLL RegisterOGRODS();
void CPL_DLL RegisterOGRXLSX();
void CPL_DLL RegisterOGRElastic();
void DeclareDeferredOGRElasticPlugin();
void CPL_DLL RegisterOGRGeoPackage();
void CPL_DLL RegisterOGRCarto();
void DeclareDeferredOGRCartoPlugin();
void CPL_DLL RegisterOGRAmigoCloud();
void CPL_DLL RegisterOGRSXF();
void CPL_DLL RegisterOGROpenFileGDB();
void DeclareDeferredOGROpenFileGDBPlugin();
void CPL_DLL RegisterOGRSelafin();
void CPL_DLL RegisterOGRJML();
void CPL_DLL RegisterOGRPLSCENES();
void DeclareDeferredOGRPLSCENESPlugin();
void CPL_DLL RegisterOGRCSW();
void CPL_DLL RegisterOGRMongoDBv3();
void DeclareDeferredOGRMongoDBv3Plugin();
void CPL_DLL RegisterOGRVDV();
void CPL_DLL RegisterOGRGMLAS();
void DeclareDeferredOGRGMLASPlugin();
void CPL_DLL RegisterOGRMVT();
void CPL_DLL RegisterOGRNGW();
void CPL_DLL RegisterOGRMapML();
void CPL_DLL RegisterOGRLVBAG();
void CPL_DLL RegisterOGRHANA();
void DeclareDeferredOGRHANAPlugin();
void CPL_DLL RegisterOGRParquet();
void DeclareDeferredOGRParquetPlugin();
void CPL_DLL RegisterOGRArrow();
void DeclareDeferredOGRArrowPlugin();
void CPL_DLL RegisterOGRGTFS();
void CPL_DLL RegisterOGRPMTiles();
void CPL_DLL RegisterOGRJSONFG();
void CPL_DLL RegisterOGRMiraMon();
void CPL_DLL RegisterOGRXODR();
void DeclareDeferredOGRXODRPlugin();
void CPL_DLL RegisterOGRADBC();
void DeclareDeferredOGRADBCPlugin();
void CPL_DLL RegisterOGRAIVector();
// @endcond

CPL_C_END

#endif /* ndef OGRSF_FRMTS_H_INCLUDED */
