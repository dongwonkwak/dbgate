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

# ─── Dependencies ───────────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

# ─── Source files ───────────────────────────────────────────────────────────
# Will be populated as modules are implemented
set(DBGATE_SOURCES
    src/main.cpp
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
    # logger — DON-23 Phase 2 stub
    src/logger/structured_logger.cpp
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
    src/logger/structured_logger.cpp
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

include(GoogleTest)
gtest_discover_tests(dbgate_tests)
