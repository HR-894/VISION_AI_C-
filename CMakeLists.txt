cmake_minimum_required(VERSION 3.20)
project(VISION_DAEMON VERSION 4.0.0 LANGUAGES CXX)

# ============================================================
# C++20 Standard
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# Options
# ============================================================
option(VISION_ENABLE_WHISPER  "Enable whisper.cpp speech-to-text"  ON)
option(VISION_ENABLE_OCR      "Enable Tesseract OCR"               ON)
option(VISION_ENABLE_AUDIO    "Enable PortAudio capture"           ON)

# ============================================================
# Dependencies — gRPC & Protobuf
# ============================================================
# gRPC engine compilation blocks as requested
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

# Setup Proto generation
get_target_property(grpc_cpp_plugin gRPC::grpc_cpp_plugin LOCATION)

set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/vision_daemon.proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# Generate cpp and grpc cpp files
add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/vision_daemon.pb.cc"
           "${PROTO_GEN_DIR}/vision_daemon.pb.h"
           "${PROTO_GEN_DIR}/vision_daemon.grpc.pb.cc"
           "${PROTO_GEN_DIR}/vision_daemon.grpc.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc>
    ARGS --cpp_out="${PROTO_GEN_DIR}"
         --grpc_out="${PROTO_GEN_DIR}"
         --plugin=protoc-gen-grpc="${grpc_cpp_plugin}"
         -I "${CMAKE_SOURCE_DIR}/proto"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
)

# ============================================================
# Dependencies — OpenCV (optional, for OCR/template matching)
# ============================================================
if(VISION_ENABLE_OCR)
    find_package(OpenCV QUIET COMPONENTS core imgproc highgui)
    if(NOT OpenCV_FOUND)
        message(WARNING "OpenCV not found — OCR disabled")
        set(VISION_ENABLE_OCR OFF)
    endif()
endif()

# ============================================================
# Dependencies — Tesseract (optional)
# ============================================================
if(VISION_ENABLE_OCR)
    find_package(Tesseract QUIET)
    if(NOT Tesseract_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(Tesseract QUIET tesseract)
        endif()
        if(NOT Tesseract_FOUND)
            message(WARNING "Tesseract not found — OCR disabled")
            set(VISION_ENABLE_OCR OFF)
        endif()
    endif()
endif()

# ============================================================
# Dependencies — Others
# ============================================================
# whisper.cpp
if(VISION_ENABLE_WHISPER AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/CMakeLists.txt")
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
endif()

# spdlog
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt")
    add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
else()
    find_package(spdlog QUIET)
endif()

# PortAudio
if(VISION_ENABLE_AUDIO AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/portaudio/CMakeLists.txt")
    set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/portaudio EXCLUDE_FROM_ALL)
endif()

# nlohmann/json
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
endif()

# ============================================================
# Source files (Stripped Cognitive & UI Logic)
# ============================================================
set(VISION_HEADERS
    include/core/config_manager.h
    include/automation/device_profiler.h
    include/automation/window_manager.h
    include/automation/system_commands.h
    include/automation/file_manager.h
    include/automation/ui_automation.h
    include/vision/screen_observer.h
    include/core/doctor.h
    include/core/safety_guard.h
    include/daemon/grpc_server.h
)

set(VISION_SOURCES
    src/main.cpp
    src/daemon/grpc_server.cpp
    src/core/config_manager.cpp
    src/automation/device_profiler.cpp
    src/automation/window_manager.cpp
    src/automation/system_commands.cpp
    src/automation/file_manager.cpp
    src/automation/ui_automation.cpp
    src/vision/screen_observer.cpp
    src/core/doctor.cpp
    src/core/safety_guard.cpp
    ${PROTO_GEN_DIR}/vision_daemon.pb.cc
    ${PROTO_GEN_DIR}/vision_daemon.grpc.pb.cc
)

if(VISION_ENABLE_WHISPER)
    list(APPEND VISION_HEADERS
        include/voice/WhisperEngine.h
        include/voice/AudioCapture.h
        include/voice/VoiceManager.h
    )
    list(APPEND VISION_SOURCES
        src/voice/WhisperEngine.cpp
        src/voice/AudioCapture.cpp
        src/voice/VoiceManager.cpp
    )
endif()

# ============================================================
# Main executable
# ============================================================
add_executable(vision_daemon ${VISION_SOURCES} ${VISION_HEADERS})

target_include_directories(vision_daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/voice
    ${CMAKE_SOURCE_DIR}/include/vision
    ${CMAKE_SOURCE_DIR}/include/automation
    ${CMAKE_SOURCE_DIR}/include/core
    ${PROTO_GEN_DIR}
)

# ============================================================
# Link libraries
# ============================================================
target_link_libraries(vision_daemon PRIVATE
    gRPC::grpc++
    gRPC::grpc
    protobuf::libprotobuf
)

if(TARGET nlohmann_json)
    target_link_libraries(vision_daemon PRIVATE nlohmann_json)
endif()

# Win32 system libraries
target_link_libraries(vision_daemon PRIVATE
    user32 gdi32 shell32 ole32 oleaut32
    winmm comsuppw wbemuuid
    pdh setupapi advapi32 iphlpapi
    dwmapi powrprof gdiplus psapi shlwapi
    wlanapi crypt32 oleacc
    dxgi d3d11 uiautomationcore
)

# spdlog
if(TARGET spdlog::spdlog)
    target_link_libraries(vision_daemon PRIVATE spdlog::spdlog)
    target_compile_definitions(vision_daemon PRIVATE VISION_HAS_SPDLOG=1)
endif()

# whisper.cpp
if(VISION_ENABLE_WHISPER)
    target_link_libraries(vision_daemon PRIVATE whisper)
    target_compile_definitions(vision_daemon PRIVATE VISION_HAS_WHISPER=1)
endif()

# OpenCV + Tesseract
if(VISION_ENABLE_OCR)
    target_link_libraries(vision_daemon PRIVATE ${OpenCV_LIBS})
    if(TARGET Tesseract::libtesseract)
        target_link_libraries(vision_daemon PRIVATE Tesseract::libtesseract)
    elseif(Tesseract_LIBRARIES)
        target_link_libraries(vision_daemon PRIVATE ${Tesseract_LIBRARIES})
        target_include_directories(vision_daemon PRIVATE ${Tesseract_INCLUDE_DIRS})
    endif()
    target_compile_definitions(vision_daemon PRIVATE VISION_HAS_OCR=1)
endif()

# PortAudio
if(VISION_ENABLE_AUDIO)
    if(TARGET portaudio_static)
        target_link_libraries(vision_daemon PRIVATE portaudio_static)
    elseif(TARGET portaudio)
        target_link_libraries(vision_daemon PRIVATE portaudio)
    endif()
    target_compile_definitions(vision_daemon PRIVATE VISION_HAS_AUDIO=1)
endif()

# ============================================================
# Compile definitions & options
# ============================================================
target_compile_definitions(vision_daemon PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    VISION_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(vision_daemon PRIVATE /W4 /utf-8 /permissive- /EHa /Zc:__cplusplus)
else()
    target_compile_options(vision_daemon PRIVATE -Wall -Wextra -Wpedantic)
endif()
