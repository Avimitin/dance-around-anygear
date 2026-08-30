cmake_minimum_required(VERSION 3.24)

project(dance_around_anygear VERSION 0.4.0 LANGUAGES C CXX)

set(ANYGEAR_NLOHMANN_JSON_HEADER
    "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
set(ANYGEAR_NLOHMANN_JSON_SHA256
    "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
if(NOT EXISTS "${ANYGEAR_NLOHMANN_JSON_HEADER}")
    message(FATAL_ERROR "Vendored nlohmann/json header is absent")
endif()
file(SHA256 "${ANYGEAR_NLOHMANN_JSON_HEADER}"
    ANYGEAR_NLOHMANN_JSON_ACTUAL_SHA256)
if(NOT ANYGEAR_NLOHMANN_JSON_ACTUAL_SHA256 STREQUAL
   ANYGEAR_NLOHMANN_JSON_SHA256)
    message(FATAL_ERROR
        "Vendored nlohmann/json v3.12.0 header hash mismatch")
endif()

option(ANYGEAR_BUILD_TESTS "Build native smoke and hardware harnesses" ON)
option(ANYGEAR_BUILD_KINECT "Build the Kinect for Windows v1 selector" ON)
option(ANYGEAR_BUILD_WEBCAM "Build the MediaPipe USB webcam selector" ON)
option(ANYGEAR_BUILD_STEAMVR "Build the SteamVR tracked-pose selector" ON)
option(ANYGEAR_BUILD_D4XX
    "Build the native RealSense D4xx bridge for the original VP4U" ON)
option(ANYGEAR_BUILD_D4XX_MEDIAPIPE_EXPERIMENTAL
    "Build the previous D4xx IR/MediaPipe VP4U replacement" ON)
option(ANYGEAR_BUILD_D4XX_SPIKE
    "Build the experimental D4xx depth/SPiKE VP4U replacement" ON)

if(ANYGEAR_BUILD_KINECT)
    set(ANYGEAR_KINECT_SDK_ROOT "$ENV{KINECTSDK10_DIR}" CACHE PATH
        "Kinect for Windows SDK 1.8 installation root")
    find_path(ANYGEAR_KINECT_INCLUDE_DIR
        NAMES NuiApi.h
        HINTS "${ANYGEAR_KINECT_SDK_ROOT}/inc"
    )
    if(NOT ANYGEAR_KINECT_INCLUDE_DIR)
        message(FATAL_ERROR
            "Kinect for Windows SDK 1.8 headers were not found. Install the SDK, "
            "set ANYGEAR_KINECT_SDK_ROOT, or configure ANYGEAR_BUILD_KINECT=OFF.")
    endif()
endif()

if(ANYGEAR_BUILD_STEAMVR)
    set(_ANYGEAR_OPENVR_DEFAULT "${CMAKE_SOURCE_DIR}/.deps/openvr/v2.15.6")
    if(DEFINED ENV{ANYGEAR_OPENVR_SDK_ROOT} AND
       NOT "$ENV{ANYGEAR_OPENVR_SDK_ROOT}" STREQUAL "")
        set(_ANYGEAR_OPENVR_DEFAULT "$ENV{ANYGEAR_OPENVR_SDK_ROOT}")
    endif()
    set(ANYGEAR_OPENVR_SDK_ROOT
        "${_ANYGEAR_OPENVR_DEFAULT}" CACHE PATH
        "OpenVR SDK v2.15.6 source root")
    find_path(ANYGEAR_OPENVR_INCLUDE_DIR
        NAMES openvr_capi.h
        HINTS "${ANYGEAR_OPENVR_SDK_ROOT}/headers"
    )
    if(NOT ANYGEAR_OPENVR_INCLUDE_DIR)
        message(FATAL_ERROR
            "OpenVR v2.15.6 headers were not found. Run "
            "tools/bootstrap-openvr.ps1 -Download, set "
            "ANYGEAR_OPENVR_SDK_ROOT, or configure "
            "ANYGEAR_BUILD_STEAMVR=OFF.")
    endif()
endif()

if(ANYGEAR_BUILD_D4XX OR ANYGEAR_BUILD_D4XX_MEDIAPIPE_EXPERIMENTAL OR
   ANYGEAR_BUILD_D4XX_SPIKE)
    set(_ANYGEAR_LIBREALSENSE_DEFAULT
        "${CMAKE_SOURCE_DIR}/.deps/librealsense/v2.50.0/include")
    if(DEFINED ENV{ANYGEAR_LIBREALSENSE_INCLUDE_DIR} AND
       NOT "$ENV{ANYGEAR_LIBREALSENSE_INCLUDE_DIR}" STREQUAL "")
        set(_ANYGEAR_LIBREALSENSE_DEFAULT
            "$ENV{ANYGEAR_LIBREALSENSE_INCLUDE_DIR}")
    endif()
    set(ANYGEAR_LIBREALSENSE_INCLUDE_DIR
        "${_ANYGEAR_LIBREALSENSE_DEFAULT}" CACHE PATH
        "Directory containing librealsense2/rs.h")
    if(NOT EXISTS
       "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}/librealsense2/rs.h")
        message(FATAL_ERROR
            "librealsense C headers were not found. Run the pinned D4xx "
            "bootstrap, set ANYGEAR_LIBREALSENSE_INCLUDE_DIR, or configure "
            "ANYGEAR_BUILD_D4XX=OFF.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ANYGEAR_COMMON_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
)

if(MINGW)
    add_link_options(
        -static
        -static-libgcc
        -static-libstdc++
        -Wl,--no-insert-timestamp
    )
endif()

set(ANYGEAR_VP4U_SOURCES
    src/vp4u/vp4uwrap.cpp
    src/vp4u/webcam.cpp
    src/vp4u/pose.cpp
    src/vp4u/body_prediction.cpp
)

function(anygear_add_selector target backend_define output_name)
    # A release backend is intentionally monolithic: the same DLL is both the
    # Spice -k module and the VP4U implementation returned to the host. This keeps
    # Spice's load list to one project-owned entry DLL per backend.
    set(sensor_sources
        src/vp4u/kinect_unavailable.cpp
        src/vp4u/steamvr_unavailable.cpp
        src/vp4u/d4xx_unavailable.cpp
        src/vp4u/spike_unavailable.cpp
    )
    if("${backend_define}" STREQUAL "ANYGEAR_BACKEND_KINECT=1")
        set(sensor_sources
            src/vp4u/kinect.cpp
            src/vp4u/steamvr_unavailable.cpp
            src/vp4u/d4xx_unavailable.cpp
            src/vp4u/spike_unavailable.cpp
        )
    elseif("${backend_define}" STREQUAL "ANYGEAR_BACKEND_STEAMVR=1")
        set(sensor_sources
            src/vp4u/kinect_unavailable.cpp
            src/vp4u/steamvr.cpp
            src/vp4u/steamvr_pose.cpp
            src/vp4u/d4xx_unavailable.cpp
            src/vp4u/spike_unavailable.cpp
        )
    elseif("${backend_define}" STREQUAL "ANYGEAR_BACKEND_D4XX=1")
        set(sensor_sources
            src/vp4u/kinect_unavailable.cpp
            src/vp4u/steamvr_unavailable.cpp
            src/vp4u/d4xx.cpp
            src/d4xx/ir_bgr.cpp
            src/vp4u/spike_unavailable.cpp
        )
    elseif("${backend_define}" STREQUAL "ANYGEAR_BACKEND_D4XX_SPIKE=1")
        set(sensor_sources
            src/vp4u/kinect_unavailable.cpp
            src/vp4u/steamvr_unavailable.cpp
            src/vp4u/d4xx.cpp
            src/vp4u/spike.cpp
            src/vp4u/spike_pose.cpp
            src/d4xx/ir_bgr.cpp
            src/d4xx/spike_ipc.cpp
        )
    endif()
    add_library(${target} SHARED
        src/plugin/backend_loader.cpp
        ${ANYGEAR_VP4U_SOURCES}
        ${sensor_sources}
    )
    target_include_directories(${target} PRIVATE
        include
        src
    )
    target_include_directories(${target} SYSTEM PRIVATE
        third_party
    )
    if("${backend_define}" STREQUAL "ANYGEAR_BACKEND_KINECT=1")
        target_include_directories(${target} SYSTEM PRIVATE
            "${ANYGEAR_KINECT_INCLUDE_DIR}"
        )
    endif()
    if("${backend_define}" STREQUAL "ANYGEAR_BACKEND_STEAMVR=1")
        target_include_directories(${target} SYSTEM PRIVATE
            "${ANYGEAR_OPENVR_INCLUDE_DIR}"
        )
    endif()
    if("${backend_define}" STREQUAL "ANYGEAR_BACKEND_D4XX=1" OR
       "${backend_define}" STREQUAL "ANYGEAR_BACKEND_D4XX_SPIKE=1")
        target_include_directories(${target} SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
        )
    endif()
    target_compile_definitions(${target} PRIVATE
        ${backend_define}
        ANYGEAR_MONOLITHIC=1
        ANYGEAR_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(${target} PRIVATE
        ${ANYGEAR_COMMON_WARNINGS}
        $<$<CXX_COMPILER_ID:GNU>:-g2>
        $<$<CXX_COMPILER_ID:GNU>:-fno-omit-frame-pointer>
        $<$<CXX_COMPILER_ID:GNU>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>
        $<$<CXX_COMPILER_ID:GNU>:-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.>
        $<$<CXX_COMPILER_ID:GNU>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.build>
        $<$<CXX_COMPILER_ID:GNU>:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=.build>
    )
    target_link_libraries(${target} PRIVATE
        mfplat
        mf
        mfreadwrite
        mfuuid
        ole32
    )
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${output_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

if(ANYGEAR_BUILD_KINECT)
    anygear_add_selector(
        anygear_kinect
        ANYGEAR_BACKEND_KINECT=1
        dance_around_anygear_kinect
    )
endif()

if(ANYGEAR_BUILD_WEBCAM)
    anygear_add_selector(
        anygear_webcam
        ANYGEAR_BACKEND_WEBCAM=1
        dance_around_anygear_webcam
    )
endif()

if(ANYGEAR_BUILD_STEAMVR)
    anygear_add_selector(
        anygear_steamvr
        ANYGEAR_BACKEND_STEAMVR=1
        dance_around_anygear_steamvr
    )
endif()

if(ANYGEAR_BUILD_D4XX)
    add_library(anygear_d4xx SHARED
        src/plugin/d4xx_native_bridge.cpp
        src/d4xx/ir_bgr.cpp
    )
    target_include_directories(anygear_d4xx PRIVATE
        include
        src
    )
    target_include_directories(anygear_d4xx SYSTEM PRIVATE
        third_party
        "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
    )
    target_compile_definitions(anygear_d4xx PRIVATE
        ANYGEAR_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(anygear_d4xx PRIVATE
        ${ANYGEAR_COMMON_WARNINGS}
        $<$<CXX_COMPILER_ID:GNU>:-g2>
        $<$<CXX_COMPILER_ID:GNU>:-fno-omit-frame-pointer>
        $<$<CXX_COMPILER_ID:GNU>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>
        $<$<CXX_COMPILER_ID:GNU>:-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.>
        $<$<CXX_COMPILER_ID:GNU>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.build>
        $<$<CXX_COMPILER_ID:GNU>:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=.build>
    )
    set_target_properties(anygear_d4xx PROPERTIES
        PREFIX ""
        OUTPUT_NAME "dance_around_anygear_d4xx"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

if(ANYGEAR_BUILD_D4XX_MEDIAPIPE_EXPERIMENTAL)
    anygear_add_selector(
        anygear_d4xx_mediapipe_experimental
        ANYGEAR_BACKEND_D4XX=1
        dance_around_anygear_d4xx_mediapipe_experimental
    )
endif()

if(ANYGEAR_BUILD_D4XX_SPIKE)
    anygear_add_selector(
        anygear_d4xx_spike
        ANYGEAR_BACKEND_D4XX_SPIKE=1
        dance_around_anygear_d4xx_spike
    )
endif()

if(ANYGEAR_BUILD_TESTS)
    enable_testing()

    add_executable(anygear_vp4u_harness tests/harness.cpp)
    target_include_directories(anygear_vp4u_harness PRIVATE src)
    target_compile_options(anygear_vp4u_harness PRIVATE ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_vp4u_harness PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(anygear_loader_smoke tests/loader_smoke.cpp)
    target_include_directories(anygear_loader_smoke PRIVATE include)
    target_compile_options(anygear_loader_smoke PRIVATE ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_loader_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(ANYGEAR_BUILD_KINECT)
        add_executable(anygear_kinect_probe
            tests/kinect_probe.cpp
            src/vp4u/kinect.cpp
            src/vp4u/webcam.cpp
        )
        target_include_directories(anygear_kinect_probe PRIVATE src)
        target_include_directories(anygear_kinect_probe SYSTEM PRIVATE
            "${ANYGEAR_KINECT_INCLUDE_DIR}"
        )
        target_compile_options(anygear_kinect_probe PRIVATE ${ANYGEAR_COMMON_WARNINGS})
        target_link_libraries(anygear_kinect_probe PRIVATE
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
        )
        set_target_properties(anygear_kinect_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()

    if(ANYGEAR_BUILD_STEAMVR)
        add_executable(anygear_steamvr_probe
            tests/steamvr_probe.cpp
            src/vp4u/steamvr.cpp
            src/vp4u/steamvr_pose.cpp
        )
        target_include_directories(anygear_steamvr_probe PRIVATE src)
        target_include_directories(anygear_steamvr_probe SYSTEM PRIVATE
            "${ANYGEAR_OPENVR_INCLUDE_DIR}"
        )
        target_compile_options(anygear_steamvr_probe PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        set_target_properties(anygear_steamvr_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()

    if(ANYGEAR_BUILD_D4XX_MEDIAPIPE_EXPERIMENTAL)
        add_executable(anygear_d4xx_probe
            tests/d4xx_probe.cpp
            src/vp4u/d4xx.cpp
            src/vp4u/webcam.cpp
            src/vp4u/pose.cpp
            src/d4xx/ir_bgr.cpp
        )
        target_include_directories(anygear_d4xx_probe PRIVATE src)
        target_include_directories(anygear_d4xx_probe SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
        )
        target_compile_options(anygear_d4xx_probe PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        target_link_libraries(anygear_d4xx_probe PRIVATE
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
        )
        set_target_properties(anygear_d4xx_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )

        add_executable(anygear_d4xx_depth_record
            tests/d4xx_depth_record.cpp
            src/vp4u/d4xx.cpp
            src/vp4u/webcam.cpp
            src/d4xx/ir_bgr.cpp
        )
        target_include_directories(anygear_d4xx_depth_record PRIVATE
            src
            third_party
        )
        target_include_directories(anygear_d4xx_depth_record SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
        )
        target_compile_options(anygear_d4xx_depth_record PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        target_link_libraries(anygear_d4xx_depth_record PRIVATE
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
        )
        set_target_properties(anygear_d4xx_depth_record PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()

    if(ANYGEAR_BUILD_KINECT AND ANYGEAR_BUILD_D4XX_SPIKE)
        add_executable(anygear_d4xx_kinect_teacher_record
            tests/d4xx_kinect_teacher_record.cpp
            src/vp4u/d4xx.cpp
            src/vp4u/kinect.cpp
            src/vp4u/webcam.cpp
            src/d4xx/ir_bgr.cpp
        )
        target_include_directories(anygear_d4xx_kinect_teacher_record PRIVATE
            src
            third_party
        )
        target_include_directories(anygear_d4xx_kinect_teacher_record
            SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
            "${ANYGEAR_KINECT_INCLUDE_DIR}"
        )
        target_compile_definitions(anygear_d4xx_kinect_teacher_record PRIVATE
            ANYGEAR_VERSION="${PROJECT_VERSION}"
        )
        target_compile_options(anygear_d4xx_kinect_teacher_record PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        target_link_libraries(anygear_d4xx_kinect_teacher_record PRIVATE
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
        )
        set_target_properties(anygear_d4xx_kinect_teacher_record PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
        add_test(NAME anygear_d4xx_kinect_teacher_record_self_test
            COMMAND anygear_d4xx_kinect_teacher_record --self-test)
    endif()

    add_executable(anygear_webcam_probe
        tests/webcam_probe.cpp
        src/vp4u/webcam.cpp
    )
    target_include_directories(anygear_webcam_probe PRIVATE src)
    target_compile_options(anygear_webcam_probe PRIVATE ${ANYGEAR_COMMON_WARNINGS})
    target_link_libraries(anygear_webcam_probe PRIVATE
        mfplat
        mf
        mfreadwrite
        mfuuid
        ole32
    )
    set_target_properties(anygear_webcam_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(anygear_mediapipe_probe
        tests/mediapipe_probe.cpp
        src/vp4u/pose.cpp
    )
    target_include_directories(anygear_mediapipe_probe PRIVATE src)
    target_compile_options(anygear_mediapipe_probe PRIVATE ${ANYGEAR_COMMON_WARNINGS})
    target_link_libraries(anygear_mediapipe_probe PRIVATE
        ole32
        windowscodecs
    )
    set_target_properties(anygear_mediapipe_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(anygear_spike_ipc_protocol_test
        tests/spike_ipc_protocol_test.cpp
    )
    target_include_directories(anygear_spike_ipc_protocol_test PRIVATE src)
    target_compile_options(anygear_spike_ipc_protocol_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_spike_ipc_protocol_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME anygear_spike_ipc_protocol
        COMMAND anygear_spike_ipc_protocol_test)

    add_executable(anygear_pose_tracking_state_test
        tests/pose_tracking_state_test.cpp
    )
    target_include_directories(anygear_pose_tracking_state_test PRIVATE src)
    target_compile_options(anygear_pose_tracking_state_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_pose_tracking_state_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME anygear_pose_tracking_state
        COMMAND anygear_pose_tracking_state_test)

    add_executable(anygear_body_prediction_test
        tests/body_prediction_test.cpp
        src/vp4u/body_prediction.cpp
    )
    target_include_directories(anygear_body_prediction_test PRIVATE src)
    target_compile_options(anygear_body_prediction_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_body_prediction_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME anygear_body_prediction
        COMMAND anygear_body_prediction_test)

    add_executable(anygear_spike_pose_test
        tests/spike_pose_test.cpp
        src/vp4u/spike_pose.cpp
    )
    target_include_directories(anygear_spike_pose_test PRIVATE src)
    target_compile_options(anygear_spike_pose_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_spike_pose_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME anygear_spike_pose_mapping
        COMMAND anygear_spike_pose_test)

    add_executable(anygear_spike_ipc_host_test
        tests/spike_ipc_host_test.cpp
        src/d4xx/spike_ipc.cpp
    )
    target_include_directories(anygear_spike_ipc_host_test PRIVATE src)
    target_compile_options(anygear_spike_ipc_host_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_spike_ipc_host_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(anygear_spike_ipc_replay
        tests/spike_ipc_replay.cpp
        src/d4xx/spike_ipc.cpp
    )
    target_include_directories(anygear_spike_ipc_replay PRIVATE src)
    target_include_directories(anygear_spike_ipc_replay SYSTEM PRIVATE
        third_party)
    target_compile_options(anygear_spike_ipc_replay PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_spike_ipc_replay PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(ANYGEAR_BUILD_D4XX_SPIKE)
        add_executable(anygear_d4xx_spike_viewer_host
            tools/native/d4xx_spike_viewer_host.cpp
            src/d4xx/spike_ipc.cpp
            src/d4xx/ir_bgr.cpp
            src/vp4u/d4xx.cpp
            src/vp4u/webcam.cpp
        )
        target_include_directories(anygear_d4xx_spike_viewer_host PRIVATE
            src
            third_party
        )
        target_include_directories(anygear_d4xx_spike_viewer_host
            SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}"
        )
        target_compile_options(anygear_d4xx_spike_viewer_host PRIVATE
            ${ANYGEAR_COMMON_WARNINGS}
        )
        target_link_options(anygear_d4xx_spike_viewer_host PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-municode>
        )
        target_link_libraries(anygear_d4xx_spike_viewer_host PRIVATE
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
            shell32
        )
        set_target_properties(anygear_d4xx_spike_viewer_host PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()

    add_executable(anygear_steamvr_pose_test
        tests/steamvr_pose_test.cpp
        src/vp4u/steamvr_pose.cpp
    )
    target_include_directories(anygear_steamvr_pose_test PRIVATE src)
    target_compile_options(anygear_steamvr_pose_test PRIVATE
        ${ANYGEAR_COMMON_WARNINGS})
    set_target_properties(anygear_steamvr_pose_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME anygear_steamvr_pose_mapping
        COMMAND anygear_steamvr_pose_test)

    if(ANYGEAR_BUILD_D4XX)
        add_executable(anygear_d4xx_ir_bgr_test
            tests/d4xx_ir_bgr_test.cpp
            src/d4xx/ir_bgr.cpp
        )
        target_include_directories(anygear_d4xx_ir_bgr_test PRIVATE src)
        target_compile_options(anygear_d4xx_ir_bgr_test PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        set_target_properties(anygear_d4xx_ir_bgr_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
        add_test(NAME anygear_d4xx_ir_bgr_conversion
            COMMAND anygear_d4xx_ir_bgr_test)

        set(_ANYGEAR_D4XX_TEST_RUNTIME
            "${CMAKE_BINARY_DIR}/test-d4xx-native")
        add_library(anygear_fake_realsense SHARED
            tests/fake_realsense.cpp)
        target_include_directories(anygear_fake_realsense SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}")
        target_compile_options(anygear_fake_realsense PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        set_target_properties(anygear_fake_realsense PROPERTIES
            PREFIX ""
            OUTPUT_NAME "realsense2"
            RUNTIME_OUTPUT_DIRECTORY "${_ANYGEAR_D4XX_TEST_RUNTIME}"
            ARCHIVE_OUTPUT_DIRECTORY "${_ANYGEAR_D4XX_TEST_RUNTIME}"
        )

        add_executable(anygear_d4xx_reconnect_test
            tests/d4xx_reconnect_test.cpp
            src/vp4u/d4xx.cpp
            src/d4xx/ir_bgr.cpp
        )
        target_include_directories(anygear_d4xx_reconnect_test PRIVATE src)
        target_include_directories(anygear_d4xx_reconnect_test SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}")
        target_compile_options(anygear_d4xx_reconnect_test PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        add_dependencies(anygear_d4xx_reconnect_test anygear_fake_realsense)
        set_target_properties(anygear_d4xx_reconnect_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
        add_test(NAME anygear_d4xx_transport_recovery
            COMMAND anygear_d4xx_reconnect_test
                $<TARGET_FILE:anygear_fake_realsense>)

        add_library(anygear_fake_vp4u SHARED
            tests/fake_visionposewrapper.cpp)
        target_include_directories(anygear_fake_vp4u PRIVATE
            src)
        target_include_directories(anygear_fake_vp4u SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}")
        target_compile_options(anygear_fake_vp4u PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        target_link_libraries(anygear_fake_vp4u PRIVATE
            anygear_fake_realsense)
        set_target_properties(anygear_fake_vp4u PROPERTIES
            PREFIX ""
            OUTPUT_NAME "visionposewrapper"
            RUNTIME_OUTPUT_DIRECTORY "${_ANYGEAR_D4XX_TEST_RUNTIME}"
        )

        add_library(anygear_fake_unity_loader SHARED
            tests/fake_unity_loader.cpp)
        target_compile_options(anygear_fake_unity_loader PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        set_target_properties(anygear_fake_unity_loader PROPERTIES
            PREFIX ""
            OUTPUT_NAME "UnityPlayer"
            RUNTIME_OUTPUT_DIRECTORY "${_ANYGEAR_D4XX_TEST_RUNTIME}"
        )

        add_executable(anygear_d4xx_native_bridge_smoke
            tests/d4xx_native_bridge_smoke.cpp)
        target_include_directories(anygear_d4xx_native_bridge_smoke PRIVATE
            include
            src)
        target_include_directories(anygear_d4xx_native_bridge_smoke SYSTEM PRIVATE
            "${ANYGEAR_LIBREALSENSE_INCLUDE_DIR}")
        target_compile_options(anygear_d4xx_native_bridge_smoke PRIVATE
            ${ANYGEAR_COMMON_WARNINGS})
        add_dependencies(anygear_d4xx_native_bridge_smoke
            anygear_fake_unity_loader)
        set_target_properties(anygear_d4xx_native_bridge_smoke PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
        add_test(NAME anygear_d4xx_native_bridge_contract
            COMMAND anygear_d4xx_native_bridge_smoke
                $<TARGET_FILE:anygear_d4xx>
                $<TARGET_FILE:anygear_fake_vp4u>)
        add_test(NAME anygear_d4xx_native_bridge_legacy_host_contract
            COMMAND anygear_d4xx_native_bridge_smoke
                $<TARGET_FILE:anygear_d4xx>
                $<TARGET_FILE:anygear_fake_vp4u>
                --legacy-host)
    endif()

    if(ANYGEAR_BUILD_KINECT)
        add_test(
            NAME anygear_loader_fallback
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_kinect>
        )
        add_test(
            NAME anygear_loader_spice_sdk
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_kinect> --sdk-hook
        )
        add_test(
            NAME anygear_vp4u_abi
            COMMAND anygear_vp4u_harness $<TARGET_FILE:anygear_kinect> --abi-only
        )
    endif()
    if(ANYGEAR_BUILD_WEBCAM)
        add_test(
            NAME anygear_webcam_loader_fallback
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_webcam>
        )
        add_test(
            NAME anygear_webcam_loader_spice_sdk
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_webcam> --sdk-hook
        )
        add_test(
            NAME anygear_webcam_vp4u_abi
            COMMAND anygear_vp4u_harness $<TARGET_FILE:anygear_webcam> --abi-only
        )
    endif()
    if(ANYGEAR_BUILD_STEAMVR)
        add_test(
            NAME anygear_steamvr_loader_fallback
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_steamvr>
        )
        add_test(
            NAME anygear_steamvr_loader_spice_sdk
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_steamvr> --sdk-hook
        )
        add_test(
            NAME anygear_steamvr_vp4u_abi
            COMMAND anygear_vp4u_harness $<TARGET_FILE:anygear_steamvr> --abi-only
        )
    endif()
    if(ANYGEAR_BUILD_D4XX_MEDIAPIPE_EXPERIMENTAL)
        add_test(
            NAME anygear_d4xx_mediapipe_loader_fallback
            COMMAND anygear_loader_smoke
                $<TARGET_FILE:anygear_d4xx_mediapipe_experimental>
        )
        add_test(
            NAME anygear_d4xx_mediapipe_loader_spice_sdk
            COMMAND anygear_loader_smoke
                $<TARGET_FILE:anygear_d4xx_mediapipe_experimental> --sdk-hook
        )
        add_test(
            NAME anygear_d4xx_mediapipe_vp4u_abi
            COMMAND anygear_vp4u_harness
                $<TARGET_FILE:anygear_d4xx_mediapipe_experimental> --abi-only
        )
    endif()
    if(ANYGEAR_BUILD_D4XX_SPIKE)
        add_test(
            NAME anygear_d4xx_spike_loader_fallback
            COMMAND anygear_loader_smoke $<TARGET_FILE:anygear_d4xx_spike>
        )
        add_test(
            NAME anygear_d4xx_spike_loader_spice_sdk
            COMMAND anygear_loader_smoke
                $<TARGET_FILE:anygear_d4xx_spike> --sdk-hook
        )
        add_test(
            NAME anygear_d4xx_spike_vp4u_abi
            COMMAND anygear_vp4u_harness $<TARGET_FILE:anygear_d4xx_spike>
                --abi-only
        )
    endif()
endif()
