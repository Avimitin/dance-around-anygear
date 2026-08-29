// VP4U compatibility declarations shared by the plugin and its test harness.
#ifndef VP4U_ABI_H
#define VP4U_ABI_H

#include <cstddef>
#include <cstdint>

namespace vp4u {

// Callback signatures.
typedef void (*OnLogCallback)(void* callbackContext, const char* logText);
typedef void (*OnVideoFrameCallback)(void* callbackContext, uint64_t timestamp,
                                     struct ImageDesc* imageDesc);
typedef void (*OnCalibrationStatusCallback)(void* callbackContext, int status,
                                            int capturedCount, int totalCountToCapture,
                                            const char* statusMessage);
typedef void (*OnPoseFrameCallback)(
    void* callbackContext,
    uint64_t timestamp,
    struct ParsedBody* pose3dArray, int pose3dCount,
    struct ParsedBody* pose2dArray, int pose2dCount,
    struct ParsedBody* mainPose2dArray, int mainPose2dCount,
    struct ParsedBody* subPose2dArray, int subPose2dCount,
    struct ImageDesc* mainCameraImage,
    struct ImageDesc* subCameraImage);

// Data layouts.
#pragma pack(push, 8)
struct ImageDesc {
    void* PointerToFrame;
    int   Width;
    int   Height;
    int   BitsPerPixel;
    int   BytesPerPixel;
    int   BytesPerRow;
    int   BytesPerFrame;
};

struct Joint {
    int   TrackingState; // 0=NotTracked 1=Inferred 2=Tracked
    float PositionX, PositionY, PositionZ;
};

constexpr int kJointCount = 30;

struct ParsedBody {
    int     TrackingState;
    int     _padding;
    int64_t TrackingId;
    Joint   JointData[kJointCount];
};
#pragma pack(pop)

// Keep these invariants explicit so every compiler produces the same ABI.
static_assert(sizeof(Joint) == 16, "VP4U Joint ABI must remain 16 bytes");
static_assert(offsetof(ParsedBody, JointData) == 16,
              "VP4U ParsedBody joints must start at byte 16");
static_assert(sizeof(ParsedBody) == 496,
              "VP4U ParsedBody ABI must remain 16 + 30*16 bytes");

// FunctionPointerTable order is part of the memory layout.
struct FunctionPointerTable {
    uint64_t (*GetCurrentTimestamp)(void);                                                  // 0
    intptr_t (*RegisterLoggerCallback)(OnLogCallback cb, void* ctx);                        // 1
    void     (*UnregisterLoggerCallback)(intptr_t handle);                                  // 2
    void     (*AddRefImageDesc)(ImageDesc* p);                                              // 3
    void     (*ReleaseImageDesc)(ImageDesc* p);                                             // 4
    intptr_t (*Initialize)(void);                                                           // 5
    void     (*Shutdown)(void);                                                             // 6
    void     (*LoadConfig)(const char* appConfigPath);                                      // 7
    void     (*ReloadConfig)(void);                                                         // 8
    bool     (*OpenRealSense)(void);                                                        // 9
    bool     (*RealSenseLeftIsActive)(void);                                                // 10
    bool     (*RealSenseRightIsActive)(void);                                               // 11
    bool     (*TryRebootRealSense)(bool forceReboot);                                       // 12
    bool     (*TryRebuildRealSenseFilters)(void);                                           // 13
    int      (*InitVisionPoseRealtime)(const char* productKey);                             // 14
    int      (*InitVisionPoseOffline)(const char* productKey);                              // 15
    bool     (*StartRealtimeCameraPreview)(OnVideoFrameCallback leftCb, void* leftCtx,      // 16
                                           OnVideoFrameCallback rightCb, void* rightCtx);
    void     (*StopRealtimeCameraPreview)(void);                                            // 17
    bool     (*StartStereoCameraCalibration)(OnCalibrationStatusCallback progressCb,        // 18
                                             OnVideoFrameCallback previewCb, void* ctx);
    bool     (*CommitStereoCameraCalibration)(void);                                        // 19
    void     (*CloseStereoCameraCalibration)(void);                                         // 20
    bool     (*StartRealtimeAnalysis)(OnPoseFrameCallback cb, void* ctx);                   // 21
    void     (*StopRealtimeAnalysis)(void);                                                 // 22
    bool     (*PrepareVideoRecording)(const char* path);                                    // 23
    bool     (*StartVideoRecording)(void);                                                  // 24
    bool     (*FinalizeVideoRecording)(void);                                               // 25
    bool     (*DeleteRecordedVideo)(const char* path);                                      // 26
    bool     (*StartOfflineAnalysis)(const char* path, OnPoseFrameCallback cb, void* ctx);  // 27
    void     (*StopOfflineAnalysis)(void);                                                  // 28
};

constexpr int kApiVersion = 210; // 0xd2

// Joint indices.
enum JointType {
    JT_SpineBase = 0, JT_SpineMid, JT_Neck, JT_Head,
    JT_ShoulderLeft, JT_ElbowLeft, JT_WristLeft, JT_HandLeft,
    JT_ShoulderRight, JT_ElbowRight, JT_WristRight, JT_HandRight,
    JT_HipLeft, JT_KneeLeft, JT_AnkleLeft, JT_FootLeft,
    JT_HipRight, JT_KneeRight, JT_AnkleRight, JT_FootRight,
    JT_SpineShoulder, JT_HandTipLeft, JT_ThumbLeft, JT_HandTipRight,
    JT_ThumbRight, JT_Nose, JT_EyeLeft, JT_EyeRight, JT_EarLeft, JT_EarRight
};

// Exported functions.
extern "C" {
int  vp4uGetVersion();
bool vp4uPreboot(const char* prebootConfigJson, OnLogCallback loggerCb, void* loggerCtx);
void vp4uShutdown();
bool vp4uGetApiTable(int apiVersion, FunctionPointerTable* table,
                     OnLogCallback loggerCb, void* loggerCtx);
}

} // namespace vp4u

#endif // VP4U_ABI_H
