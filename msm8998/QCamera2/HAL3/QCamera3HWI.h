/* Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#ifndef __QCAMERA3HARDWAREINTERFACE_H__
#define __QCAMERA3HARDWAREINTERFACE_H__

// System dependencies
#include <CameraMetadata.h>
#include <pthread.h>
#include <utils/KeyedVector.h>
#include <utils/List.h>
#include <map>
// Camera dependencies
#include "hardware/camera3.h"
#include "QCamera3Channel.h"
#include "QCamera3CropRegionMapper.h"
#include "QCamera3HALHeader.h"
#include "QCamera3Mem.h"
#include "QCameraPerf.h"
#include "QCameraCommon.h"
#include "QCamera3VendorTags.h"
#include "QCameraDualCamSettings.h"

#include "HdrPlusClient.h"

extern "C" {
#include "mm_camera_interface.h"
#include "mm_jpeg_interface.h"
}

using ::android::hardware::camera::common::V1_0::helper::CameraMetadata;
using namespace android;

namespace qcamera {

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

/* Time related macros */
typedef int64_t nsecs_t;
#define NSEC_PER_SEC 1000000000LLU
#define NSEC_PER_USEC 1000LLU
#define NSEC_PER_33MSEC 33000000LLU

/*Orchestrate Macros */
#define EV_COMP_SETTLE_DELAY   2
#define GB_HDR_HALF_STEP_EV -6
#define GB_HDR_2X_STEP_EV 6

#define FRAME_REGISTER_LRU_SIZE 256
#define INTERNAL_FRAME_STARTING_NUMBER 800
#define EMPTY_FRAMEWORK_FRAME_NUMBER 0xFFFFFFFF

typedef enum {
    SET_ENABLE,
    SET_CONTROLENABLE,
    SET_RELOAD_CHROMATIX,
    SET_STATUS,
} optype_t;

#define MODULE_ALL 0

extern volatile uint32_t gCamHal3LogLevel;

class QCamera3MetadataChannel;
class QCamera3PicChannel;
class QCamera3HeapMemory;
class QCamera3Exif;
class ShutterDispatcher;
class BufferDispatcher;

typedef struct {
    camera3_stream_t *stream;
    camera3_stream_buffer_set_t buffer_set;
    stream_status_t status;
    int registered;
    QCamera3ProcessingChannel *channel;
} stream_info_t;

typedef struct {
    // Stream handle
    camera3_stream_t *stream;
    // Buffer handle
    buffer_handle_t *buffer;
    // Buffer status
    camera3_buffer_status_t bufStatus = CAMERA3_BUFFER_STATUS_OK;
} PendingBufferInfo;

typedef struct {
    // Frame number corresponding to request
    uint32_t frame_number;
    // Time when request queued into system
    nsecs_t timestamp;
    List<PendingBufferInfo> mPendingBufferList;
    bool hdrplus;
} PendingBuffersInRequest;

class PendingBuffersMap {
public:
    // Number of outstanding buffers at flush
    uint32_t numPendingBufsAtFlush;
    // List of pending buffers per request
    List<PendingBuffersInRequest> mPendingBuffersInRequest;
    uint32_t get_num_overall_buffers();
    void removeBuf(buffer_handle_t *buffer);
    int32_t getBufErrStatus(buffer_handle_t *buffer);
};

class FrameNumberRegistry {
public:

    FrameNumberRegistry();
    ~FrameNumberRegistry();
    int32_t allocStoreInternalFrameNumber(uint32_t frameworkFrameNumber,
            uint32_t &internalFrameNumber);
    int32_t generateStoreInternalFrameNumber(uint32_t &internalFrameNumber);
    int32_t freeInternalFrameNumber(uint32_t internalFrameNumber);
    int32_t getFrameworkFrameNumber(uint32_t internalFrameNumber, uint32_t &frameworkFrameNumber);
    void purgeOldEntriesLocked();

private:
    std::map<uint32_t, uint32_t> _register;
    uint32_t _nextFreeInternalNumber;
    Mutex mRegistryLock;
};

class QCamera3HardwareInterface;

/*
 * ShutterDispatcher class dispatches shutter callbacks in order of the frame
 * number. It will dispatch a shutter callback only after all shutter callbacks
 * of previous frames were dispatched.
 */
class ShutterDispatcher {
public:
    ShutterDispatcher(QCamera3HardwareInterface *parent);
    virtual ~ShutterDispatcher() = default;

    // Tell dispatch to expect a shutter for a frame number.
    void expectShutter(uint32_t frameNumber);
    // Mark a shutter callback for a frame ready.
    void markShutterReady(uint32_t frameNumber, uint64_t timestamp);
    // Discard a pending shutter for frame number.
    void clear(uint32_t frameNumber);
    // Discard all pending shutters.
    void clear();

private:
    struct Shutter {
        bool ready; // If the shutter is ready.
        uint64_t timestamp; // Timestamp of the shutter.
        Shutter() : ready(false), timestamp(0) {};
    };

    std::mutex mLock;

    // frame number -> shutter map. Protected by mLock.
    std::map<uint32_t, Shutter> mShutters;

    QCamera3HardwareInterface *mParent;
};

/*
 * BufferDispatcher class dispatches output buffers in a stream in order of the
 * frame number. It will dispatch an output buffer in a stream only after all
 * previous output buffers in the same stream were dispatched.
 */
class OutputBufferDispatcher {
public:
    OutputBufferDispatcher(QCamera3HardwareInterface *parent);
    virtual ~OutputBufferDispatcher() = default;

    // Configure streams.
    status_t configureStreams(camera3_stream_configuration_t *streamList);
    // Tell dispatcher to expect a buffer for a stream for a frame number.
    status_t expectBuffer(uint32_t frameNumber, camera3_stream_t *stream);
    // Mark a buffer ready for a stream for a frame number.
    void markBufferReady(uint32_t frameNumber, const camera3_stream_buffer_t &buffer);
    // Discard all pending buffers. If clearConfiguredStreams is true, discard configured streams
    // as well.
    void clear(bool clearConfiguredStreams = true);

private:
    struct Buffer {
        bool ready; // If the buffer is ready.
        camera3_stream_buffer_t buffer;
        Buffer() : ready(false), buffer({}) {};
    };

    std::mutex mLock;

    // A two-level map: stream -> (frame number -> buffer). Protected by mLock.
    std::map<camera3_stream_t*, std::map<uint32_t, Buffer>> mStreamBuffers;

    QCamera3HardwareInterface *mParent;
};

class QCamera3HardwareInterface : HdrPlusClientListener {
public:
    /* static variable and functions accessed by camera service */
    static camera3_device_ops_t mCameraOps;
    //Id of each session in bundle/link
    static uint32_t sessionId[MM_CAMERA_MAX_NUM_SENSORS];
    static int initialize(const struct camera3_device *,
                const camera3_callback_ops_t *callback_ops);
    static int configure_streams(const struct camera3_device *,
                camera3_stream_configuration_t *stream_list);
    static const camera_metadata_t* construct_default_request_settings(
                                const struct camera3_device *, int type);
    static int process_capture_request(const struct camera3_device *,
                                camera3_capture_request_t *request);

    static void dump(const struct camera3_device *, int fd);
    static int flush(const struct camera3_device *);
    static int close_camera_device(struct hw_device_t* device);

public:
    QCamera3HardwareInterface(uint32_t cameraId,
            const camera_module_callbacks_t *callbacks);
    virtual ~QCamera3HardwareInterface();
    static void camEvtHandle(uint32_t camera_handle, mm_camera_event_t *evt,
                                          void *user_data);
    int openCamera(struct hw_device_t **hw_device);
    camera_metadata_t* translateCapabilityToMetadata(int type);

    typedef struct {
        camera3_stream_t *stream;
        bool need_metadata;
        bool meteringOnly;
    } InternalRequest;

    static int getCamInfo(uint32_t cameraId, struct camera_info *info);
    static cam_capability_t *getCapabilities(mm_camera_ops_t *ops,
            uint32_t cam_handle);
    static int initCapabilities(uint32_t cameraId);
    static int initStaticMetadata(uint32_t cameraId);
    static int initHdrPlusClientLocked();
    static void makeTable(cam_dimension_t *dimTable, size_t size,
            size_t max_size, int32_t *sizeTable);
    static void makeFPSTable(cam_fps_range_t *fpsTable, size_t size,
            size_t max_size, int32_t *fpsRangesTable);
    static void makeOverridesList(cam_scene_mode_overrides_t *overridesTable,
            size_t size, size_t max_size, uint8_t *overridesList,
            uint8_t *supported_indexes, uint32_t camera_id);
    static size_t filterJpegSizes(int32_t *jpegSizes, int32_t *processedSizes,
            size_t processedSizesCnt, size_t maxCount, cam_rect_t active_array_size,
            uint8_t downscale_factor);
    static void convertToRegions(cam_rect_t rect, int32_t* region, int weight);
    static void convertFromRegions(cam_area_t &roi, const CameraMetadata &frame_settings,
                                   uint32_t tag);
    static bool resetIfNeededROI(cam_area_t* roi, const cam_crop_region_t* scalerCropRegion);
    static int32_t getSensorSensitivity(int32_t iso_mode);

    double computeNoiseModelEntryS(int32_t sensitivity);
    double computeNoiseModelEntryO(int32_t sensitivity);

    static void captureResultCb(mm_camera_super_buf_t *metadata,
                camera3_stream_buffer_t *buffer, uint32_t frame_number,
                bool isInputBuffer, void *userdata);

    int initialize(const camera3_callback_ops_t *callback_ops);
    int configureStreams(camera3_stream_configuration_t *stream_list);
    int configureStreamsPerfLocked(camera3_stream_configuration_t *stream_list);
    int processCaptureRequest(camera3_capture_request_t *request,
                              List<InternalRequest> &internalReqs);
    int orchestrateRequest(camera3_capture_request_t *request);
    void orchestrateResult(camera3_capture_result_t *result);
    void orchestrateNotify(camera3_notify_msg_t *notify_msg);

    void dump(int fd);
    int flushPerf();

    int setFrameParameters(camera3_capture_request_t *request,
            cam_stream_ID_t streamID, int blob_request, uint32_t snapshotStreamId);
    int32_t setReprocParameters(camera3_capture_request_t *request,
            metadata_buffer_t *reprocParam, uint32_t snapshotStreamId);
    int translateToHalMetadata(const camera3_capture_request_t *request,
            metadata_buffer_t *parm, uint32_t snapshotStreamId);
    int translateFwkMetadataToHalMetadata(const camera_metadata_t *frameworkMetadata,
            metadata_buffer_t *hal_metadata, uint32_t snapshotStreamId, int64_t minFrameDuration);
    camera_metadata_t* translateCbUrgentMetadataToResultMetadata (
                             metadata_buffer_t *metadata, bool lastUrgentMetadataInBatch);
    camera_metadata_t* translateFromHalMetadata(metadata_buffer_t *metadata,
                            nsecs_t timestamp, int32_t request_id,
                            const CameraMetadata& jpegMetadata, uint8_t pipeline_depth,
                            uint8_t capture_intent,
                            uint8_t hybrid_ae_enable,
                            /* DevCamDebug metadata translateFromHalMetadata augment .h */
                            uint8_t DevCamDebug_meta_enable,
                            /* DevCamDebug metadata end */
                            bool pprocDone, uint8_t fwk_cacMode,
                            bool lastMetadataInBatch,
                            const bool *enableZsl);
    camera_metadata_t* saveRequestSettings(const CameraMetadata& jpegMetadata,
                            camera3_capture_request_t *request);
    int initParameters();
    void deinitParameters();
    QCamera3ReprocessChannel *addOfflineReprocChannel(const reprocess_config_t &config,
            QCamera3ProcessingChannel *inputChHandle);
    bool needRotationReprocess();
    bool needJpegExifRotation();
    bool needReprocess(cam_feature_mask_t postprocess_mask);
    bool needJpegRotation();
    cam_denoise_process_type_t getWaveletDenoiseProcessPlate();
    cam_denoise_process_type_t getTemporalDenoiseProcessPlate();

    void captureResultCb(mm_camera_super_buf_t *metadata,
                camera3_stream_buffer_t *buffer, uint32_t frame_number,
                bool isInputBuffer);
    cam_dimension_t calcMaxJpegDim();
    bool needOnlineRotation();
    uint32_t getJpegQuality();
    QCamera3Exif *getExifData();
    mm_jpeg_exif_params_t get3AExifParams();
    uint8_t getMobicatMask();
    static void getFlashInfo(const int cameraId,
            bool& hasFlash,
            char (&flashNode)[QCAMERA_MAX_FILEPATH_LENGTH]);
    const char *getEepromVersionInfo();
    const uint32_t *getLdafCalib();
    void get3AVersion(cam_q3a_version_t &swVersion);
    static void setBufferErrorStatus(QCamera3Channel*, uint32_t frameNumber,
            camera3_buffer_status_t err, void *userdata);
    void setBufferErrorStatus(QCamera3Channel*, uint32_t frameNumber,
            camera3_buffer_status_t err);
    bool is60HzZone();

    // Get dual camera related info
    bool isDeviceLinked() {return mIsDeviceLinked;}
    bool isMainCamera() {return mIsMainCamera;}
    uint32_t getSensorMountAngle();
    const cam_related_system_calibration_data_t *getRelatedCalibrationData();

    template <typename fwkType, typename halType> struct QCameraMap {
        fwkType fwk_name;
        halType hal_name;
    };

    typedef struct {
        const char *const desc;
        cam_cds_mode_type_t val;
    } QCameraPropMap;

private:

    // State transition conditions:
    // "\" means not applicable
    // "x" means not valid
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |            |  CLOSED  |  OPENED  | INITIALIZED | CONFIGURED | STARTED | ERROR | DEINIT |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |  CLOSED    |    \     |   open   |     x       |    x       |    x    |   x   |   x    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |  OPENED    |  close   |    \     | initialize  |    x       |    x    | error |   x    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |INITIALIZED |  close   |    x     |     \       | configure  |   x     | error |   x    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // | CONFIGURED |  close   |    x     |     x       | configure  | request | error |   x    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |  STARTED   |  close   |    x     |     x       | configure  |    \    | error |   x    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |   ERROR    |  close   |    x     |     x       |     x      |    x    |   \   |  any   |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+
    // |   DEINIT   |  close   |    x     |     x       |     x      |    x    |   x   |   \    |
    // +------------+----------+----------+-------------+------------+---------+-------+--------+

    typedef enum {
        CLOSED,
        OPENED,
        INITIALIZED,
        CONFIGURED,
        STARTED,
        ERROR,
        DEINIT
    } State;

    int openCamera();
    int closeCamera();
    int flush(bool restartChannels);
    static size_t calcMaxJpegSize(uint32_t camera_id);
    cam_dimension_t getMaxRawSize(uint32_t camera_id);
    static void addStreamConfig(Vector<int32_t> &available_stream_configs,
            int32_t scalar_format, const cam_dimension_t &dim,
            int32_t config_type);

    int validateCaptureRequest(camera3_capture_request_t *request,
                               List<InternalRequest> &internallyRequestedStreams);
    int validateStreamDimensions(camera3_stream_configuration_t *streamList);
    int validateStreamRotations(camera3_stream_configuration_t *streamList);
    int validateUsageFlags(const camera3_stream_configuration_t *streamList);
    int validateUsageFlagsForEis(const camera3_stream_configuration_t *streamList);
    void deriveMinFrameDuration();
    void handleBuffersDuringFlushLock(camera3_stream_buffer_t *buffer);
    int64_t getMinFrameDuration(const camera3_capture_request_t *request);
    void handleMetadataWithLock(mm_camera_super_buf_t *metadata_buf,
            bool free_and_bufdone_meta_buf,
            bool lastUrgentMetadataInBatch,
            bool lastMetadataInBatch,
            bool *p_is_metabuf_queued);
    void handleBatchMetadata(mm_camera_super_buf_t *metadata_buf,
            bool free_and_bufdone_meta_buf);
    void handleBufferWithLock(camera3_stream_buffer_t *buffer,
            uint32_t frame_number);
    void handleInputBufferWithLock(uint32_t frame_number);
    // Handle pending results when a new result metadata of a frame is received.
    // metadata callbacks are invoked in the order of frame number.
    void handlePendingResultMetadataWithLock(uint32_t frameNumber,
            const camera_metadata_t *resultMetadata);
    void handleDepthDataLocked(const cam_depth_data_t &depthData,
            uint32_t frameNumber);
    void notifyErrorFoPendingDepthData(QCamera3DepthChannel *depthCh);
    void unblockRequestIfNecessary();
    void dumpMetadataToFile(tuning_params_t &meta, uint32_t &dumpFrameCount,
            bool enabled, const char *type, uint32_t frameNumber);
    static void getLogLevel();
    static int32_t getPDStatIndex(cam_capability_t *caps);

    void cleanAndSortStreamInfo();
    void extractJpegMetadata(CameraMetadata& jpegMetadata,
            const camera3_capture_request_t *request);

    bool isSupportChannelNeeded(camera3_stream_configuration_t *streamList,
            cam_stream_size_info_t stream_config_info);
    bool isHdrSnapshotRequest(camera3_capture_request *request);
    int32_t setMobicat();

    int32_t getSensorModeInfo(cam_sensor_mode_info_t &sensorModeInfo);
    int32_t setHalFpsRange(const CameraMetadata &settings,
            metadata_buffer_t *hal_metadata);
    int32_t extractSceneMode(const CameraMetadata &frame_settings, uint8_t metaMode,
            metadata_buffer_t *hal_metadata);
    int32_t setVideoHdrMode(metadata_buffer_t *hal_metadata,
            cam_video_hdr_mode_t vhdr);
    int32_t numOfSizesOnEncoder(const camera3_stream_configuration_t *streamList,
            const cam_dimension_t &maxViewfinderSize);

    void addToPPFeatureMask(int stream_format, uint32_t stream_idx);
    void updateFpsInPreviewBuffer(metadata_buffer_t *metadata, uint32_t frame_number);
    void updateTimeStampInPendingBuffers(uint32_t frameNumber, nsecs_t timestamp);

    void enablePowerHint();
    void disablePowerHint();
    int32_t dynamicUpdateMetaStreamInfo();
    int32_t startAllChannels();
    int32_t stopAllChannels();
    int32_t notifyErrorForPendingRequests();
    void notifyError(uint32_t frameNumber,
            camera3_error_msg_code_t errorCode);
    int32_t getReprocessibleOutputStreamId(uint32_t &id);
    int32_t handleCameraDeviceError();

    bool isOnEncoder(const cam_dimension_t max_viewfinder_size,
            uint32_t width, uint32_t height);
    void hdrPlusPerfLock(mm_camera_super_buf_t *metadata_buf);

    static bool supportBurstCapture(uint32_t cameraId);
    int32_t setBundleInfo();
    int32_t setInstantAEC(const CameraMetadata &meta);

    static void convertLandmarks(cam_face_landmarks_info_t face, int32_t* landmarks);
    static void setInvalidLandmarks(int32_t* landmarks);

    static void setPAAFSupport(cam_feature_mask_t& feature_mask,
            cam_stream_type_t stream_type,
            cam_color_filter_arrangement_t filter_arrangement);
    int32_t setSensorHDR(metadata_buffer_t *hal_metadata, bool enable,
            bool isVideoHdrEnable = false);

    template <typename T>
    static void adjustBlackLevelForCFA(T input[BLACK_LEVEL_PATTERN_CNT],
            T output[BLACK_LEVEL_PATTERN_CNT],
            cam_color_filter_arrangement_t color_arrangement);

    camera3_device_t   mCameraDevice;
    uint32_t           mCameraId;
    mm_camera_vtbl_t  *mCameraHandle;
    bool               mCameraInitialized;
    camera_metadata_t *mDefaultMetadata[CAMERA3_TEMPLATE_COUNT];
    const camera3_callback_ops_t *mCallbackOps;

    QCamera3MetadataChannel *mMetadataChannel;
    QCamera3PicChannel *mPictureChannel;
    QCamera3RawChannel *mRawChannel;
    QCamera3SupportChannel *mSupportChannel;
    QCamera3SupportChannel *mAnalysisChannel;
    QCamera3RawDumpChannel *mRawDumpChannel;
    QCamera3HdrPlusRawSrcChannel *mHdrPlusRawSrcChannel;
    QCamera3RegularChannel *mDummyBatchChannel;
    QCamera3DepthChannel *mDepthChannel;
    QCameraPerfLockMgr mPerfLockMgr;

    uint32_t mChannelHandle;

    void saveExifParams(metadata_buffer_t *metadata);
    mm_jpeg_exif_params_t mExifParams;

     //First request yet to be processed after configureStreams
    bool mFirstConfiguration;
    bool mFlush;
    bool mFlushPerf;
    bool mEnableRawDump;
    bool mForceHdrSnapshot;
    QCamera3HeapMemory *mParamHeap;
    metadata_buffer_t* mParameters;
    metadata_buffer_t* mPrevParameters;
    CameraMetadata mCurJpegMeta;
    bool m_bIsVideo;
    bool m_bIs4KVideo;
    bool m_bEisSupportedSize;
    bool m_bEisEnable;
    bool m_bEis3PropertyEnabled;
    bool m_bEisSupported;
    typedef struct {
        cam_dimension_t dim;
        int format;
        uint32_t usage;
    } InputStreamInfo;

    InputStreamInfo mInputStreamInfo;
    uint8_t m_MobicatMask;
    uint8_t m_bTnrEnabled;
    int8_t  mSupportedFaceDetectMode;
    uint8_t m_bTnrPreview;
    uint8_t m_bSwTnrPreview;
    uint8_t m_bTnrVideo;
    uint8_t m_debug_avtimer;
    uint8_t m_bVideoHdrEnabled;
    uint8_t m_cacModeDisabled;

    /* Data structure to store pending request */
    typedef struct {
        camera3_stream_t *stream;
        camera3_stream_buffer_t *buffer;
        // metadata needs to be consumed by the corresponding stream
        // in order to generate the buffer.
        bool need_metadata;
    } RequestedBufferInfo;

    typedef struct {
        uint32_t frame_number;
        uint32_t num_buffers;
        int32_t request_id;
        List<RequestedBufferInfo> buffers;
        List<InternalRequest> internalRequestList;
        int blob_request;
        uint8_t bUseFirstPartial; // Use first available partial result in case of jumpstart.
        nsecs_t timestamp;
        camera3_stream_buffer_t *input_buffer;
        const camera_metadata_t *settings;
        const camera_metadata_t *resultMetadata; // Result metadata for this request.
        CameraMetadata jpegMetadata;
        uint8_t pipeline_depth;
        uint32_t partial_result_cnt;
        uint8_t capture_intent;
        uint8_t fwkCacMode;
        uint8_t hybrid_ae_enable;
        /* DevCamDebug metadata PendingRequestInfo */
        uint8_t DevCamDebug_meta_enable;
        /* DevCamDebug metadata end */

        bool enableZsl; // If ZSL is enabled.
        bool hdrplus; // If this is an HDR+ request.
    } PendingRequestInfo;
    typedef struct {
        uint32_t frame_number;
        uint32_t stream_ID;
    } PendingFrameDropInfo;

    class FrameNumberRegistry _orchestrationDb;
    typedef KeyedVector<uint32_t, Vector<PendingBufferInfo> > FlushMap;
    typedef List<QCamera3HardwareInterface::PendingRequestInfo>::iterator
            pendingRequestIterator;
    typedef List<QCamera3HardwareInterface::RequestedBufferInfo>::iterator
            pendingBufferIterator;

    List<PendingRequestInfo> mPendingRequestsList;
    List<PendingFrameDropInfo> mPendingFrameDropList;
    /* Use last frame number of the batch as key and first frame number of the
     * batch as value for that key */
    KeyedVector<uint32_t, uint32_t> mPendingBatchMap;
    cam_stream_ID_t mBatchedStreamsArray;

    PendingBuffersMap mPendingBuffersMap;
    pthread_cond_t mRequestCond;
    uint32_t mPendingLiveRequest;
    bool mWokenUpByDaemon;
    int32_t mCurrentRequestId;
    cam_stream_size_info_t mStreamConfigInfo;

    ShutterDispatcher mShutterDispatcher;
    OutputBufferDispatcher mOutputBufferDispatcher;

    //mutex for serialized access to camera3_device_ops_t functions
    pthread_mutex_t mMutex;

    //condition used to signal flush after buffers have returned
    pthread_cond_t mBuffersCond;

    List<stream_info_t*> mStreamInfo;

    int64_t mMinProcessedFrameDuration;
    int64_t mMinJpegFrameDuration;
    int64_t mMinRawFrameDuration;

    uint32_t mMetaFrameCount;
    bool    mUpdateDebugLevel;
    const camera_module_callbacks_t *mCallbacks;

    uint8_t mCaptureIntent;
    uint8_t mCacMode;
    uint8_t mHybridAeEnable;
    // DevCamDebug metadata internal variable
    uint8_t mDevCamDebugMetaEnable;
    /* DevCamDebug metadata end */

    metadata_buffer_t mReprocMeta; //scratch meta buffer
    /* 0: Not batch, non-zero: Number of image buffers in a batch */
    uint8_t mBatchSize;
    // Used only in batch mode
    uint8_t mToBeQueuedVidBufs;
    // Fixed video fps
    float mHFRVideoFps;
public:
    uint32_t mOpMode;
    bool mStreamConfig;
    QCameraCommon   mCommon;
private:
    uint32_t mFirstFrameNumberInBatch;
    camera3_stream_t mDummyBatchStream;
    bool mNeedSensorRestart;
    bool mPreviewStarted;
    uint32_t mMinInFlightRequests;
    uint32_t mMaxInFlightRequests;
    bool mPDSupported;
    int32_t mPDIndex;
    // Param to trigger instant AEC.
    bool mInstantAEC;
    // Param to know when to reset AEC
    bool mResetInstantAEC;
    // Frame number, untill which we need to drop the frames.
    uint32_t mInstantAECSettledFrameNumber;
    // Max number of frames, that HAL will hold without displaying, for instant AEC mode.
    uint8_t mAecSkipDisplayFrameBound;
    // Counter to keep track of number of frames that took for AEC convergence.
    uint8_t mInstantAecFrameIdxCount;
    /* sensor output size with current stream configuration */
    QCamera3CropRegionMapper mCropRegionMapper;

    cam_feature_mask_t mCurrFeatureState;
    /* Ldaf calibration data */
    bool mLdafCalibExist;
    uint32_t mLdafCalib[2];
    int32_t mLastCustIntentFrmNum;

    static const QCameraMap<camera_metadata_enum_android_control_effect_mode_t,
            cam_effect_mode_type> EFFECT_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_control_awb_mode_t,
            cam_wb_mode_type> WHITE_BALANCE_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_control_scene_mode_t,
            cam_scene_mode_type> SCENE_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_control_af_mode_t,
            cam_focus_mode_type> FOCUS_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_color_correction_aberration_mode_t,
            cam_aberration_mode_t> COLOR_ABERRATION_MAP[];
    static const QCameraMap<camera_metadata_enum_android_control_ae_antibanding_mode_t,
            cam_antibanding_mode_type> ANTIBANDING_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_lens_state_t,
            cam_af_lens_state_t> LENS_STATE_MAP[];
    static const QCameraMap<camera_metadata_enum_android_control_ae_mode_t,
            cam_flash_mode_t> AE_FLASH_MODE_MAP[];
    static const QCameraMap<camera_metadata_enum_android_flash_mode_t,
            cam_flash_mode_t> FLASH_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_statistics_face_detect_mode_t,
            cam_face_detect_mode_t> FACEDETECT_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_lens_info_focus_distance_calibration_t,
            cam_focus_calibration_t> FOCUS_CALIBRATION_MAP[];
    static const QCameraMap<camera_metadata_enum_android_sensor_test_pattern_mode_t,
            cam_test_pattern_mode_t> TEST_PATTERN_MAP[];
    static const QCameraMap<camera_metadata_enum_android_video_hdr_mode_t,
            cam_video_hdr_mode_t> VIDEO_HDR_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_sensor_reference_illuminant1_t,
            cam_illuminat_t> REFERENCE_ILLUMINANT_MAP[];
    static const QCameraMap<int32_t,
            cam_hfr_mode_t> HFR_MODE_MAP[];
    static const QCameraMap<camera_metadata_enum_android_ir_mode_t,
            cam_ir_mode_type_t> IR_MODES_MAP[];
    static const QCameraMap<qcamera3_ext_instant_aec_mode_t,
            cam_aec_convergence_type> INSTANT_AEC_MODES_MAP[];
    static const QCameraMap<camera_metadata_enum_android_binning_correction_mode_t,
            cam_binning_correction_mode_t> BINNING_CORRECTION_MODES_MAP[];
    static const QCameraMap<qcamera3_ext_exposure_meter_mode_t,
            cam_auto_exposure_mode_type> AEC_MODES_MAP[];
    static const QCameraMap<qcamera3_ext_iso_mode_t,
            cam_iso_mode_type> ISO_MODES_MAP[];
    static const QCameraPropMap CDS_MAP[];

    pendingRequestIterator erasePendingRequest(pendingRequestIterator i);
    //GPU library to read buffer padding details.
    void *lib_surface_utils;
    int (*LINK_get_surface_pixel_alignment)();
    uint32_t mSurfaceStridePadding;

    bool mFirstMetadataCallback;
    void sendPartialMetadataWithLock(metadata_buffer_t *metadata,
            const pendingRequestIterator requestIter,
            bool lastUrgentMetadataInBatch);

    State mState;
    //Dual camera related params
    bool mIsDeviceLinked;
    bool mIsMainCamera;
    uint8_t mLinkedCameraId;
    QCamera3HeapMemory *m_pDualCamCmdHeap;
    cam_dual_camera_cmd_info_t *m_pDualCamCmdPtr;
    cam_sync_related_sensors_event_info_t m_relCamSyncInfo;
    Mutex mFlushLock;
    bool m60HzZone;

    // Stream IDs used in stream configuration with HDR+ client.
    const static uint32_t kPbRaw10InputStreamId = 0;
    const static uint32_t kPbYuvOutputStreamId = 1;
    const static uint32_t kPbRaw16OutputStreamId = 2;

    // Issue an additional RAW for every 10 requests to control RAW capture rate. Requesting RAW
    // too often will cause frame drops due to latency of sending RAW to HDR+ service.
    const static uint32_t kHdrPlusRawPeriod = 10;

    // Define a pending HDR+ request submitted to HDR+ service and not yet received by HAL.
    struct HdrPlusPendingRequest {
        // YUV buffer from QCamera3PicChannel to be filled by HDR+ client with an HDR+ processed
        // frame.
        std::shared_ptr<mm_camera_buf_def_t> yuvBuffer;

        // Output buffers in camera framework's request.
        std::vector<camera3_stream_buffer_t> frameworkOutputBuffers;

        // Settings in camera framework's request.
        std::shared_ptr<metadata_buffer_t> settings;
    };

    // Fill pbcamera::StreamConfiguration based on the channel stream.
    status_t fillPbStreamConfig(pbcamera::StreamConfiguration *config, uint32_t pbStreamId,
            int pbStreamFormat, QCamera3Channel *channel, uint32_t streamIndex);

    // Open HDR+ client asynchronously.
    status_t openHdrPlusClientAsyncLocked();

    // Enable HDR+ mode. Easel will start capturing ZSL buffers.
    status_t enableHdrPlusModeLocked();

    // Disable HDR+ mode. Easel will stop capturing ZSL buffers.
    void disableHdrPlusModeLocked();

    // Configure streams for HDR+.
    status_t configureHdrPlusStreamsLocked();

    // Try to submit an HDR+ request. Returning true if an HDR+ request was submitted. Returning
    // false if it is not an HDR+ request or submitting an HDR+ request failed. Must be called with
    // gHdrPlusClientLock held.
    bool trySubmittingHdrPlusRequestLocked(HdrPlusPendingRequest *hdrPlusRequest,
        const camera3_capture_request_t &request, const CameraMetadata &metadata);

    // Update HDR+ result metadata with the still capture's request settings.
    void updateHdrPlusResultMetadata(CameraMetadata &resultMetadata,
            std::shared_ptr<metadata_buffer_t> settings);

    // HDR+ client callbacks.
    void onOpened(std::unique_ptr<HdrPlusClient> client) override;
    void onOpenFailed(status_t err) override;
    void onFatalError() override;
    void onCaptureResult(pbcamera::CaptureResult *result,
            const camera_metadata_t &resultMetadata) override;
    void onFailedCaptureResult(pbcamera::CaptureResult *failedResult) override;

    // Map from frame number to frame. Must be protected by mHdrPlusPendingRequestsLock.
    std::map<uint32_t, HdrPlusPendingRequest> mHdrPlusPendingRequests;
    Mutex mHdrPlusPendingRequestsLock;

    // If HDR+ mode is enabled i.e. if Easel is capturing ZSL buffers.
    bool mHdrPlusModeEnabled;

    // If ZSL is enabled (android.control.enableZsl).
    bool mZslEnabled;

    // If HAL provides RAW input buffers to Easel. This is just for prototyping.
    bool mIsApInputUsedForHdrPlus;

    // Current sensor mode information.
    cam_sensor_mode_info_t mSensorModeInfo;

    // If there is a capture request with preview intent since stream configuration.
    bool mFirstPreviewIntentSeen;

    bool m_bSensorHDREnabled;
};

}; // namespace qcamera

#endif /* __QCAMERA2HARDWAREINTERFACE_H__ */
