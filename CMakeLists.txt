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
    include/core/config_manager.h
    include/automation/device_profiler.h
    include/core/safety_guard.h
    include/automation/window_manager.h
    include/automation/system_commands.h
    include/automation/file_manager.h
    include/core/smart_template_matcher.h
    include/ai/command_router.h
    include/ai/fast_complex_handler.h
    include/memory/context_manager.h
    include/memory/agent_memory.h
    include/core/user_behavior.h
    include/core/web_search.h
    include/automation/gpu_setup_wizard.h
    include/automation/model_downloader_wizard.h
    include/automation/ui_automation.h
    include/ui/chat_widget.h
    include/ui/voice_visualizer.h
    include/vision/screen_observer.h
    include/memory/vector_memory.h
    include/core/confidence_scorer.h
    include/ui/settings_dialog.h
    include/ui/vision_ai.h
    include/core/doctor.h
    include/core/update_manager.h
    include/plugins/i_action_plugin.h
    include/plugins/plugin_loader.h
    include/memory/document_parser.h
    include/vision/timeline_logger.h
)

set(VISION_SOURCES
    src/main.cpp
    src/core/config_manager.cpp
    src/automation/device_profiler.cpp
    src/core/safety_guard.cpp
    src/automation/window_manager.cpp
    src/automation/system_commands.cpp
    src/automation/file_manager.cpp
    src/core/smart_template_matcher.cpp
    src/ai/command_router.cpp
    src/ai/fast_complex_handler.cpp
    src/memory/context_manager.cpp
    src/memory/agent_memory.cpp
    src/core/user_behavior.cpp
    src/core/web_search.cpp
    src/automation/gpu_setup_wizard.cpp
    src/automation/model_downloader_wizard.cpp
    src/automation/ui_automation.cpp
    src/ui/vision_ai.cpp
    src/ui/chat_widget.cpp
    src/ui/voice_visualizer.cpp
    src/vision/screen_observer.cpp
    src/memory/vector_memory.cpp
    src/core/confidence_scorer.cpp
    src/ui/settings_dialog.cpp
    src/core/doctor.cpp
    src/core/update_manager.cpp
    src/plugins/plugin_loader.cpp
    src/memory/document_parser.cpp
    src/vision/timeline_logger.cpp
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

if(VISION_ENABLE_LLM)
    list(APPEND VISION_HEADERS
        include/ai/ai_backend.h
        include/ai/local_backend.h
        include/ai/cloud_backend.h
        include/ai/instruction_translator.h
        include/ai/llm_controller.h
        include/automation/react_agent.h
        include/automation/action_executor.h
    )
    list(APPEND VISION_SOURCES
        src/ai/local_backend.cpp
        src/ai/cloud_backend.cpp
        src/ai/instruction_translator.cpp
        src/ai/llm_controller.cpp
        src/automation/react_agent.cpp
        src/automation/action_executor.cpp
    )
endif()

if(VISION_ENABLE_OCR)
    # OCR-specific sources (future)
endif()

# ============================================================
# Plugin shared libraries (Phase 8)
# ============================================================
if(VISION_ENABLE_LLM)
    # Web Plugin DLL
    add_library(web_plugin SHARED src/plugins/web_plugin/web_plugin.cpp)
    target_include_directories(web_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/plugins
    )
    target_link_libraries(web_plugin PRIVATE nlohmann_json shell32)
    target_compile_definitions(web_plugin PRIVATE VISION_PLUGIN_EXPORT)
    set_target_properties(web_plugin PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
    )

    # Media Plugin DLL
    add_library(media_plugin SHARED src/plugins/media_plugin/media_plugin.cpp)
    target_include_directories(media_plugin PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/plugins
    )
    target_link_libraries(media_plugin PRIVATE nlohmann_json ole32 user32)
    target_compile_definitions(media_plugin PRIVATE VISION_PLUGIN_EXPORT)
    set_target_properties(media_plugin PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
    )
endif()

# ============================================================
# Main executable
# ============================================================
qt_add_executable(VISION_AI WIN32 ${VISION_SOURCES} ${VISION_HEADERS})

# ui_automation.h starts with "ui_" which tricks Qt's AutoUic into
# looking for a non-existent "automation.ui" form file.
set_source_files_properties(
    include/automation/ui_automation.h
    src/automation/ui_automation.cpp
    src/automation/action_executor.cpp
    src/automation/window_manager.cpp
    PROPERTIES SKIP_AUTOUIC ON
)

target_include_directories(VISION_AI PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/ui
    ${CMAKE_SOURCE_DIR}/include/ai
    ${CMAKE_SOURCE_DIR}/include/voice
    ${CMAKE_SOURCE_DIR}/include/vision
    ${CMAKE_SOURCE_DIR}/include/automation
    ${CMAKE_SOURCE_DIR}/include/memory
    ${CMAKE_SOURCE_DIR}/include/core
    ${CMAKE_SOURCE_DIR}/include/plugins
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
    dxgi d3d11 uiautomationcore
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
    VISION_AI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    VISION_AI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    VISION_AI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ${VISION_GPU_DEFINE}  # CUDA / Vulkan / CPU flag
)

if(MSVC)
    target_compile_options(VISION_AI PRIVATE /W4 /utf-8 /permissive- /EHa /Zc:__cplusplus)
else()
    target_compile_options(VISION_AI PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================
# Post-build: windeployqt + asset copy
# ============================================================

# Automatically run windeployqt on the built executable
# This copies Qt DLLs, plugins (platforms/, imageformats/, tls/, styles/) into the output dir
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-translations
            --no-opengl-sw
            --no-system-d3d-compiler
            "$<TARGET_FILE:VISION_AI>"
        COMMENT "Running windeployqt to deploy Qt dependencies..."
        VERBATIM
    )
    message(STATUS "✓ windeployqt found: ${WINDEPLOYQT_EXECUTABLE}")
else()
    message(WARNING "⚠ windeployqt not found — Qt DLLs must be manually deployed")
endif()

# Copy model files
if(EXISTS "${CMAKE_SOURCE_DIR}/models")
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/models"
            "$<TARGET_FILE_DIR:VISION_AI>/models"
        COMMENT "Copying model files..."
    )
endif()

# Copy image templates
if(EXISTS "${CMAKE_SOURCE_DIR}/images")
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/images"
            "$<TARGET_FILE_DIR:VISION_AI>/images"
        COMMENT "Copying image templates..."
    )
endif()

# Create persistent data directory
add_custom_command(TARGET VISION_AI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:VISION_AI>/data"
    COMMENT "Creating data/ directory for persistent user memory..."
)

# ============================================================
# Post-build: Copy third-party DLLs to output directory
# ============================================================

# llama.cpp DLL
if(VISION_ENABLE_LLM AND TARGET llama)
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:llama>"
            "$<TARGET_FILE_DIR:VISION_AI>"
        COMMENT "Copying llama.dll..."
    )
    # Also copy ggml if it's a separate shared lib
    if(TARGET ggml)
        add_custom_command(TARGET VISION_AI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:ggml>"
                "$<TARGET_FILE_DIR:VISION_AI>"
            COMMENT "Copying ggml.dll..."
        )
    endif()
endif()

# whisper.cpp DLL
if(VISION_ENABLE_WHISPER AND TARGET whisper)
    add_custom_command(TARGET VISION_AI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:whisper>"
            "$<TARGET_FILE_DIR:VISION_AI>"
        COMMENT "Copying whisper.dll..."
    )
endif()

# PortAudio DLL
if(VISION_ENABLE_AUDIO)
    if(TARGET portaudio)
        add_custom_command(TARGET VISION_AI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:portaudio>"
                "$<TARGET_FILE_DIR:VISION_AI>"
            COMMENT "Copying portaudio.dll..."
        )
    endif()
endif()

# ============================================================
# Tests (Phase 12: Enterprise Hardening)
# ============================================================
option(VISION_BUILD_TESTS "Build Unit Tests using GoogleTest" ON)

if(VISION_BUILD_TESTS)
    enable_testing()
    
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_subdirectory(tests)
endif()

# ============================================================
# Profiling (Phase 12: Enterprise Hardening)
# ============================================================
option(VISION_ENABLE_PROFILING "Enable Tracy Frame Profiler" OFF)

if(VISION_ENABLE_PROFILING)
    FetchContent_Declare(
        tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.10.zip
    )
    FetchContent_MakeAvailable(tracy)
    target_link_libraries(VISION_AI PRIVATE TracyClient)
    target_compile_definitions(VISION_AI PRIVATE TRACY_ENABLE)
endif()

# ============================================================
# Install rules (for CPack / manual install)
# ============================================================
install(TARGETS VISION_AI RUNTIME DESTINATION bin)
install(DIRECTORY models/ DESTINATION bin/models OPTIONAL)
install(DIRECTORY images/ DESTINATION bin/images OPTIONAL)
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/data"
        DESTINATION bin/data OPTIONAL)

# Install third-party DLLs
if(VISION_ENABLE_LLM AND TARGET llama)
    install(FILES "$<TARGET_FILE:llama>" DESTINATION bin)
endif()
if(VISION_ENABLE_WHISPER AND TARGET whisper)
    install(FILES "$<TARGET_FILE:whisper>" DESTINATION bin)
endif()

# Install Qt deployed files (windeployqt output is alongside the exe)
install(DIRECTORY "$<TARGET_FILE_DIR:VISION_AI>/platforms"  DESTINATION bin OPTIONAL)
install(DIRECTORY "$<TARGET_FILE_DIR:VISION_AI>/styles"     DESTINATION bin OPTIONAL)
install(DIRECTORY "$<TARGET_FILE_DIR:VISION_AI>/imageformats" DESTINATION bin OPTIONAL)
install(DIRECTORY "$<TARGET_FILE_DIR:VISION_AI>/tls"        DESTINATION bin OPTIONAL)

# Install all .dll files from the build output (catches Qt DLLs, curl, etc.)
install(DIRECTORY "$<TARGET_FILE_DIR:VISION_AI>/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
        PATTERN "plugins" EXCLUDE)

message(STATUS "============================================")
message(STATUS "  VISION AI v${PROJECT_VERSION}")
message(STATUS "  LLM:      ${VISION_ENABLE_LLM}")
message(STATUS "  Cloud:    ${VISION_ENABLE_CLOUD}")
message(STATUS "  Whisper:  ${VISION_ENABLE_WHISPER}")
message(STATUS "  OCR:      ${VISION_ENABLE_OCR}")
message(STATUS "  Audio:    ${VISION_ENABLE_AUDIO}")
message(STATUS "  GPU:      ${VISION_GPU_BACKEND}")
message(STATUS "============================================")
