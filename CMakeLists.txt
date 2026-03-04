cmake_minimum_required(VERSION 3.25)

project(dbgate
    VERSION 0.1.0
    DESCRIPTION "DB Access Control Proxy"
    LANGUAGES CXX
)

# ─── C++ Standard ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Compiler warnings ─────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
)

# ─── Fuzzing option ──────────────────────────────────────────────────────────
option(DBGATE_ENABLE_FUZZING "Build libFuzzer fuzz targets only (requires clang)" OFF)

# ─── Dependencies ───────────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog REQUIRED)
if(NOT DBGATE_ENABLE_FUZZING)
    find_package(yaml-cpp REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(GTest REQUIRED)
endif()

if(NOT DBGATE_ENABLE_FUZZING)
# ─── Source files ───────────────────────────────────────────────────────────
# Will be populated as modules are implemented
set(DBGATE_SOURCES
    src/main.cpp
    src/common/async_stream.cpp
    src/protocol/mysql_packet.cpp
    src/protocol/handshake.cpp
    src/protocol/command.cpp
    src/proxy/session.cpp
    src/proxy/proxy_server.cpp
    src/health/health_check.cpp
    # parser — DON-23 Phase 2 stub
    src/parser/sql_parser.cpp
    src/parser/procedure_detector.cpp
    src/parser/injection_detector.cpp
    # policy — DON-23 Phase 2 stub
    src/policy/policy_engine.cpp
    src/policy/policy_loader.cpp
    src/policy/policy_version_store.cpp
    # logger — DON-23 Phase 2 stub
    src/logger/structured_logger.cpp
    # stats — DON-28
    src/stats/uds_server.cpp
)

# ─── Main executable ───────────────────────────────────────────────────────
add_executable(dbgate ${DBGATE_SOURCES})

target_include_directories(dbgate PRIVATE
    ${CMAKE_SOURCE_DIR}/src   # src/common/types.hpp 등 공통 헤더는 "common/types.hpp" 형태로 참조
)

target_link_libraries(dbgate PRIVATE
    Boost::system
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# ─── Tests ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(dbgate_tests
    tests/test_logger.cpp
    tests/test_mysql_packet.cpp
    tests/test_handshake_auth.cpp
    tests/test_sql_parser.cpp
    tests/test_injection_detector.cpp
    tests/test_policy_engine.cpp
    tests/test_stats_collector.cpp
    tests/test_uds_server.cpp
    tests/test_proxy_pipeline.cpp
    tests/test_proxy_server.cpp
    tests/test_edge_cases.cpp
    tests/test_async_stream.cpp
    tests/test_ssl_tls.cpp
    src/common/async_stream.cpp
    src/logger/structured_logger.cpp
    src/protocol/mysql_packet.cpp
    src/protocol/command.cpp
    src/protocol/handshake.cpp
    src/parser/sql_parser.cpp
    src/parser/injection_detector.cpp
    src/parser/procedure_detector.cpp
    src/policy/policy_loader.cpp
    src/policy/policy_engine.cpp
    src/policy/policy_version_store.cpp
    src/stats/uds_server.cpp
    src/health/health_check.cpp
    src/proxy/session.cpp
    src/proxy/proxy_server.cpp
)

target_include_directories(dbgate_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dbgate_tests PRIVATE
    GTest::gtest_main
    Boost::system
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

set(DBGATE_SANITIZER_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
set(DBGATE_TSAN_ENABLED OFF)
if(DBGATE_SANITIZER_FLAGS MATCHES "(^| )-fsanitize=thread( |$)")
    set(DBGATE_TSAN_ENABLED ON)
endif()

set(DBGATE_TSAN_RUNTIME_SUPPORTED ON)
if(DBGATE_TSAN_ENABLED
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(DBGATE_MMAP_RND_BITS_PATH "/proc/sys/vm/mmap_rnd_bits")
    if(EXISTS "${DBGATE_MMAP_RND_BITS_PATH}")
        file(READ "${DBGATE_MMAP_RND_BITS_PATH}" DBGATE_MMAP_RND_BITS_RAW)
        string(STRIP "${DBGATE_MMAP_RND_BITS_RAW}" DBGATE_MMAP_RND_BITS)
        if(DBGATE_MMAP_RND_BITS MATCHES "^[0-9]+$" AND DBGATE_MMAP_RND_BITS GREATER 28)
            set(DBGATE_TSAN_RUNTIME_SUPPORTED OFF)
            message(WARNING
                "Skipping gtest discovery for TSan on Linux arm64: "
                "vm.mmap_rnd_bits=${DBGATE_MMAP_RND_BITS} (> 28) crashes TSan runtime."
            )
        endif()
    endif()
endif()

if(DBGATE_TSAN_RUNTIME_SUPPORTED)
    include(GoogleTest)
    gtest_discover_tests(dbgate_tests)
else()
    add_test(
        NAME dbgate_tests_tsan_runtime_unsupported
        COMMAND ${CMAKE_COMMAND} -E echo
            "Skipping TSan unit tests on this host (Linux arm64 vm.mmap_rnd_bits > 28)."
    )
endif()

else() # DBGATE_ENABLE_FUZZING is ON

# ─── Fuzz targets ────────────────────────────────────────────────────────────
# libFuzzer provides main(); each target is a standalone executable.
# Sanitizer flags (-fsanitize=fuzzer,address) are injected via CMakePresets.json.

# fuzz_mysql_packet — protocol layer only (no spdlog)
add_executable(fuzz_mysql_packet
    tests/fuzz/fuzz_mysql_packet.cpp
    src/protocol/mysql_packet.cpp
    src/protocol/command.cpp
)
target_include_directories(fuzz_mysql_packet PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_options(fuzz_mysql_packet PRIVATE -fsanitize=fuzzer)

# fuzz_sql_parser — parser layer (spdlog for InjectionDetector)
add_executable(fuzz_sql_parser
    tests/fuzz/fuzz_sql_parser.cpp
    src/parser/sql_parser.cpp
    src/parser/injection_detector.cpp
    src/parser/procedure_detector.cpp
)
target_include_directories(fuzz_sql_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_options(fuzz_sql_parser PRIVATE -fsanitize=fuzzer)
target_link_libraries(fuzz_sql_parser PRIVATE spdlog::spdlog)
target_compile_definitions(fuzz_sql_parser PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF)

# fuzz_policy_engine — parser + policy (spdlog, no yaml-cpp)
add_executable(fuzz_policy_engine
    tests/fuzz/fuzz_policy_engine.cpp
    src/parser/sql_parser.cpp
    src/parser/injection_detector.cpp
    src/parser/procedure_detector.cpp
    src/policy/policy_engine.cpp
)
target_include_directories(fuzz_policy_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_options(fuzz_policy_engine PRIVATE -fsanitize=fuzzer)
target_link_libraries(fuzz_policy_engine PRIVATE spdlog::spdlog)
target_compile_definitions(fuzz_policy_engine PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF)

endif() # DBGATE_ENABLE_FUZZING
