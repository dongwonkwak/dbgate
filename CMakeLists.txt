cmake_minimum_required(VERSION 3.25)

project(dbgate
    VERSION 0.1.0
    DESCRIPTION "DB Access Control Proxy"
    LANGUAGES CXX
)

# ─── C++ Standard ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
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
)

# ─── Main executable ───────────────────────────────────────────────────────
add_executable(dbgate ${DBGATE_SOURCES})

target_include_directories(dbgate PRIVATE
    ${CMAKE_SOURCE_DIR}/src
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

# Will be populated as test files are added
# add_executable(dbgate_tests
#     tests/test_mysql_packet.cpp
#     tests/test_sql_parser.cpp
#     tests/test_policy_engine.cpp
#     tests/test_injection_detector.cpp
# )
#
# target_link_libraries(dbgate_tests PRIVATE
#     GTest::gtest_main
#     Boost::system
#     spdlog::spdlog
#     yaml-cpp::yaml-cpp
#     OpenSSL::SSL
#     OpenSSL::Crypto
# )
#
# include(GoogleTest)
# gtest_discover_tests(dbgate_tests)
