cmake_minimum_required(VERSION 3.20)
project(VISION_AI VERSION 3.0.0 LANGUAGES CXX)

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
option(VISION_ENABLE_LLM      "Enable llama.cpp LLM inference"     ON)
option(VISION_ENABLE_CLOUD    "Enable Groq Cloud inference (libcurl)" ON)
option(VISION_ENABLE_OCR      "Enable Tesseract OCR"               ON)
option(VISION_ENABLE_AUDIO    "Enable PortAudio capture"           ON)
option(VISION_BUILD_TESTS     "Build unit tests"                   OFF)

# ============================================================
# Dependencies — Qt6
# ============================================================
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network Concurrent)
qt_standard_project_setup()

# ============================================================
# Dependencies — OpenCV (optional, for OCR/template matching)
# ============================================================
if(VISION_ENABLE_OCR)
    find_package(OpenCV QUIET COMPONENTS core imgproc highgui)
    if(NOT OpenCV_FOUND)
        message(WARNING "OpenCV not found — OCR/template matching disabled")
        set(VISION_ENABLE_OCR OFF)
    endif()
endif()

# ============================================================
# Dependencies — Tesseract (optional)
# ============================================================
if(VISION_ENABLE_OCR)
    find_package(Tesseract QUIET)
    if(NOT Tesseract_FOUND)
        # Try pkg-config fallback
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
# GPU Backend Auto-Detection (CUDA > Vulkan > CPU)
# ============================================================
# Try CUDA first (NVIDIA → fastest path via cuBLAS/Tensor Cores)
find_package(CUDAToolkit QUIET)
find_package(Vulkan QUIET)

if(CUDAToolkit_FOUND)
    message(STATUS "✓ NVIDIA CUDA found → enabling GGML_CUDA (maximum GPU performance)")
    set(VISION_GPU_BACKEND "CUDA")
    set(GGML_CUDA    ON  CACHE BOOL "" FORCE)
    set(GGML_VULKAN  OFF CACHE BOOL "" FORCE)
    set(VISION_GPU_DEFINE "VISION_GPU_CUDA=1")  # applied after target creation
elseif(Vulkan_FOUND)
    message(STATUS "✓ Vulkan found → enabling GGML_VULKAN (AMD/Intel/NVIDIA universal)")
    set(VISION_GPU_BACKEND "Vulkan")
    set(GGML_CUDA    OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN  ON  CACHE BOOL "" FORCE)
    set(VISION_GPU_DEFINE "VISION_GPU_VULKAN=1")
else()
    message(WARNING "⚠ No GPU backend found → CPU-only mode")
    set(VISION_GPU_BACKEND "CPU (fallback)")
    set(GGML_CUDA    OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN  OFF CACHE BOOL "" FORCE)
    set(VISION_GPU_DEFINE "VISION_GPU_CPU=1")
endif()

# llama.cpp — GPU flags already set above
if(VISION_ENABLE_LLM AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
    # Fix C++20 char8_t MSVC error in llama-chat.cpp (u8"..." → const char8_t*)
    if(MSVC AND TARGET llama)
        target_compile_options(llama PRIVATE /Zc:char8_t-)
    endif()
elseif(VISION_ENABLE_LLM)
    message(WARNING "llama.cpp not found in third_party/ — LLM disabled")
    set(VISION_ENABLE_LLM OFF)
endif()

# whisper.cpp
if(VISION_ENABLE_WHISPER AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/CMakeLists.txt")
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
elseif(VISION_ENABLE_WHISPER)
    message(WARNING "whisper.cpp not found in third_party/ — Speech disabled")
    set(VISION_ENABLE_WHISPER OFF)
endif()

# spdlog
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt")
    add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
else()
    find_package(spdlog QUIET)
    if(NOT spdlog_FOUND)
        message(WARNING "spdlog not found — using fallback console logging")
    endif()
endif()

# PortAudio
if(VISION_ENABLE_AUDIO AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/portaudio/CMakeLists.txt")
    set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/portaudio EXCLUDE_FROM_ALL)
elseif(VISION_ENABLE_AUDIO)
    find_package(portaudio QUIET)
    if(NOT portaudio_FOUND)
        message(WARNING "PortAudio not found — Audio capture disabled")
        set(VISION_ENABLE_AUDIO OFF)
    endif()
endif()

# nlohmann/json (header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
else()
    find_package(nlohmann_json QUIET)
    if(NOT nlohmann_json_FOUND)
        message(FATAL_ERROR "nlohmann/json not found. Place json.hpp in third_party/nlohmann/")
    endif()
endif()

# ============================================================
# Dependencies — libcurl (for Groq Cloud Backend)
# ============================================================
if(VISION_ENABLE_CLOUD)
    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        message(WARNING "libcurl not found — Groq Cloud inference disabled (install via vcpkg: vcpkg install curl:x64-windows)")
        set(VISION_ENABLE_CLOUD OFF)
    else()
        message(STATUS "✓ libcurl found → enabling Groq Cloud backend")
    endif()
endif()

# ============================================================
# Source files
# ============================================================
set(VISION_HEADERS
    include/config_manager.h
    include/device_profiler.h
    include/safety_guard.h
    include/window_manager.h
    include/system_commands.h
    include/file_manager.h
    include/smart_template_matcher.h
    include/command_router.h
    include/fast_complex_handler.h
    include/context_manager.h
    include/agent_memory.h
    include/user_behavior.h
    include/web_search.h
    include/gpu_setup_wizard.h
    include/model_downloader_wizard.h
    include/ui_automation.h
    include/chat_widget.h
    include/screen_observer.h
    include/vector_memory.h
    include/confidence_scorer.h
    include/settings_dialog.h
    include/vision_ai.h
)

set(VISION_SOURCES
    src/main.cpp
    src/config_manager.cpp
    src/device_profiler.cpp
    src/safety_guard.cpp
    src/window_manager.cpp
    src/system_commands.cpp
    src/file_manager.cpp
    src/smart_template_matcher.cpp
    src/command_router.cpp
    src/fast_complex_handler.cpp
    src/context_manager.cpp
    src/agent_memory.cpp
    src/user_behavior.cpp
    src/web_search.cpp
    src/gpu_setup_wizard.cpp
    src/model_downloader_wizard.cpp
    src/ui_automation.cpp
    src/vision_ai.cpp
    src/chat_widget.cpp
    src/screen_observer.cpp
    src/vector_memory.cpp
    src/confidence_scorer.cpp
    src/settings_dialog.cpp
)

# Conditional AI sources
if(VISION_ENABLE_WHISPER)
    list(APPEND VISION_HEADERS include/whisper_engine.h include/audio_capture.h)
    list(APPEND VISION_SOURCES src/whisper_engine.cpp src/audio_capture.cpp)
endif()

if(VISION_ENABLE_LLM)
    list(APPEND VISION_HEADERS
        include/ai_backend.h
        include/local_backend.h
        include/cloud_backend.h
        include/instruction_translator.h
        include/llm_controller.h
        include/react_agent.h
        include/action_executor.h
    )
    list(APPEND VISION_SOURCES
        src/local_backend.cpp
        src/cloud_backend.cpp
        src/instruction_translator.cpp
        src/llm_controller.cpp
        src/react_agent.cpp
        src/action_executor.cpp
        src/doctor.cpp
    )
endif()

if(VISION_ENABLE_OCR)
    # OCR-specific sources (future)
endif()

# ============================================================
# Main executable
# ============================================================
qt_add_executable(VISION_AI WIN32 ${VISION_SOURCES} ${VISION_HEADERS})

# ui_automation.h starts with "ui_" which tricks Qt's AutoUic into
# looking for a non-existent "automation.ui" form file.
set_source_files_properties(
    include/ui_automation.h
    src/ui_automation.cpp
    src/action_executor.cpp
    src/window_manager.cpp
    PROPERTIES SKIP_AUTOUIC ON
)

target_include_directories(VISION_AI PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================
# Link libraries
# ============================================================
target_link_libraries(VISION_AI PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Network
    Qt6::Concurrent
    nlohmann_json
)

# Win32 system libraries
target_link_libraries(VISION_AI PRIVATE
    user32 gdi32 shell32 ole32 oleaut32
    winmm comsuppw wbemuuid
    pdh setupapi advapi32 iphlpapi
    dwmapi powrprof gdiplus psapi shlwapi
    wlanapi crypt32 oleacc
)

# spdlog
if(TARGET spdlog::spdlog)
    target_link_libraries(VISION_AI PRIVATE spdlog::spdlog)
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_SPDLOG=1)
endif()

# llama.cpp
if(VISION_ENABLE_LLM)
    target_link_libraries(VISION_AI PRIVATE llama)
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_LLM=1)
endif()

# libcurl (Groq Cloud Backend)
if(VISION_ENABLE_CLOUD)
    target_link_libraries(VISION_AI PRIVATE CURL::libcurl)
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_CLOUD=1)
endif()

# whisper.cpp
if(VISION_ENABLE_WHISPER)
    target_link_libraries(VISION_AI PRIVATE whisper)
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_WHISPER=1)
endif()

# OpenCV + Tesseract
if(VISION_ENABLE_OCR)
    target_link_libraries(VISION_AI PRIVATE ${OpenCV_LIBS})
    if(TARGET Tesseract::libtesseract)
        target_link_libraries(VISION_AI PRIVATE Tesseract::libtesseract)
    elseif(Tesseract_LIBRARIES)
        target_link_libraries(VISION_AI PRIVATE ${Tesseract_LIBRARIES})
        target_include_directories(VISION_AI PRIVATE ${Tesseract_INCLUDE_DIRS})
    endif()
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_OCR=1)
endif()

# PortAudio
if(VISION_ENABLE_AUDIO)
    if(TARGET portaudio_static)
        target_link_libraries(VISION_AI PRIVATE portaudio_static)
    elseif(TARGET portaudio)
        target_link_libraries(VISION_AI PRIVATE portaudio)
    endif()
    target_compile_definitions(VISION_AI PRIVATE VISION_HAS_AUDIO=1)
endif()

# ============================================================
# Compile definitions & options
# ============================================================
target_compile_definitions(VISION_AI PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    VISION_VERSION="${PROJECT_VERSION}"
    ${VISION_GPU_DEFINE}  # CUDA / Vulkan / CPU flag
)

if(MSVC)
    target_compile_options(VISION_AI PRIVATE /W4 /utf-8 /permissive- /EHa)
else()
    target_compile_options(VISION_AI PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================
# Post-build: Qt platform plugins are handled by windeployqt during deployment

if(EXISTS "${CMAKE_SOURCE_DIR}/models")
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/models"
            "$<TARGET_FILE_DIR:VISION_AI>/models"
        COMMENT "Copying model files..."
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/images")
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/images"
            "$<TARGET_FILE_DIR:VISION_AI>/images"
        COMMENT "Copying image templates..."
    )
endif()

# ============================================================
# Tests (optional)
# ============================================================
if(VISION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
# Install rules
# ============================================================
install(TARGETS VISION_AI RUNTIME DESTINATION bin)
install(DIRECTORY models/ DESTINATION bin/models OPTIONAL)
install(DIRECTORY images/ DESTINATION bin/images OPTIONAL)

message(STATUS "============================================")
message(STATUS "  VISION AI v${PROJECT_VERSION}")
message(STATUS "  LLM:      ${VISION_ENABLE_LLM}")
message(STATUS "  Cloud:    ${VISION_ENABLE_CLOUD}")
message(STATUS "  Whisper:  ${VISION_ENABLE_WHISPER}")
message(STATUS "  OCR:      ${VISION_ENABLE_OCR}")
message(STATUS "  Audio:    ${VISION_ENABLE_AUDIO}")
message(STATUS "  GPU:      ${VISION_GPU_BACKEND}")
message(STATUS "============================================")
