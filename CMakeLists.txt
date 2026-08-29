cmake_minimum_required(VERSION 3.24)

project(dance_around_anygear VERSION 0.3.0 LANGUAGES C CXX)

option(ANYGEAR_BUILD_TESTS "Build native smoke and hardware harnesses" ON)
option(ANYGEAR_BUILD_KINECT "Build the Kinect for Windows v1 selector" ON)
option(ANYGEAR_BUILD_WEBCAM "Build the MediaPipe USB webcam selector" ON)
option(ANYGEAR_BUILD_STEAMVR "Build the SteamVR tracked-pose selector" ON)

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
)

function(anygear_add_selector target backend_define output_name)
    # A release backend is intentionally monolithic: the same DLL is both the
    # Spice -k module and the VP4U implementation returned to the host. This keeps
    # Spice's load list to one project-owned entry DLL per backend.
    set(sensor_sources
        src/vp4u/kinect_unavailable.cpp
        src/vp4u/steamvr_unavailable.cpp
    )
    if("${backend_define}" STREQUAL "ANYGEAR_BACKEND_KINECT=1")
        set(sensor_sources
            src/vp4u/kinect.cpp
            src/vp4u/steamvr_unavailable.cpp
        )
    elseif("${backend_define}" STREQUAL "ANYGEAR_BACKEND_STEAMVR=1")
        set(sensor_sources
            src/vp4u/kinect_unavailable.cpp
            src/vp4u/steamvr.cpp
            src/vp4u/steamvr_pose.cpp
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
endif()
