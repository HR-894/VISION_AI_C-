cmake_minimum_required(VERSION 3.20)
project(VisionAITests)

# We link against the same objects as VISION_AI.
# Since VISION_AI is an executable, we need to gather its sources or make a static lib.
# For simplicity, we can just compile the sources we want to test directly into the test targets.

# Define common sources needed for the tests
set(TEST_COMMON_SOURCES
    ../src/memory/vector_memory.cpp
    ../src/memory/document_parser.cpp
    ../src/ai/command_router.cpp
    ../src/core/smart_template_matcher.cpp
    ../src/ai/fast_complex_handler.cpp
    ../src/core/config_manager.cpp
    ../src/core/safety_guard.cpp
)

# Common include directories
set(TEST_INCLUDE_DIRS
    ../include
    ../thirdparty/nlohmann_json/include
    ../thirdparty/spdlog/include
)

# ── test_vector_memory ──────────────────────────────────────────
add_executable(test_vector_memory test_vector_memory.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_vector_memory PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_vector_memory PRIVATE gtest_main Crypt32)
# If VISION_HAS_SPDLOG is required by the sources
target_compile_definitions(test_vector_memory PRIVATE VISION_HAS_SPDLOG)

# Add to CTest
add_test(NAME VectorMemoryTest COMMAND test_vector_memory)

# ── test_command_router ─────────────────────────────────────────
add_executable(test_command_router test_command_router.cpp ${TEST_COMMON_SOURCES})
target_include_directories(test_command_router PRIVATE ${TEST_INCLUDE_DIRS})
target_link_libraries(test_command_router PRIVATE gtest_main Crypt32)
target_compile_definitions(test_command_router PRIVATE VISION_HAS_SPDLOG)

add_test(NAME CommandRouterTest COMMAND test_command_router)
