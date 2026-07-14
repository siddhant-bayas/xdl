cmake_minimum_required(VERSION 3.16)
project(xdl VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(XDL_BUILD_CLI    "Build the xdl CLI tool"        ON)
option(XDL_BUILD_TESTS  "Build the test suite"          ON)
option(XDL_BUILD_BENCH  "Build the benchmark binary"    OFF)
option(XDL_STATIC_LINK  "Link dependencies statically"  OFF)

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

# Static linking (for release builds)
if(XDL_STATIC_LINK)
    if(NOT MSVC)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    endif()
endif()

# ---------------------------------------------------------------------------
# Optional: system LZ4 (falls back to bundled impl if not found)
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 liblz4)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(xdl_core STATIC
    src/page.cpp
    src/compression.cpp
    src/storage.cpp
    src/cache.cpp
    src/index.cpp
    src/pager.cpp
    src/db.cpp
    src/wal.cpp
    src/bptree.cpp
    src/mmap.cpp
    src/migrate.cpp
)

target_include_directories(xdl_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(xdl_core PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(xdl_core PUBLIC Threads::Threads)

if(LZ4_FOUND)
    target_compile_definitions(xdl_core PUBLIC XDL_HAVE_LZ4)
    target_include_directories(xdl_core PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(xdl_core PRIVATE ${LZ4_LIBRARIES})
    message(STATUS "XDL: using system LZ4 (${LZ4_VERSION})")
else()
    message(STATUS "XDL: using bundled LZ4 implementation")
endif()

# ---------------------------------------------------------------------------
# CLI executable
# ---------------------------------------------------------------------------
if(XDL_BUILD_CLI)
    add_executable(xdl src/main.cpp)
    target_link_libraries(xdl PRIVATE xdl_core)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(XDL_BUILD_TESTS)
    enable_testing()
    add_executable(xdl_tests tests/test_xdl.cpp)
    target_link_libraries(xdl_tests PRIVATE xdl_core)
    add_test(NAME xdl_unit_tests COMMAND xdl_tests)
endif()

# ---------------------------------------------------------------------------
# Bench
# ---------------------------------------------------------------------------
if(XDL_BUILD_BENCH)
    add_executable(xdl_bench bench/bench.cpp)
    target_link_libraries(xdl_bench PRIVATE xdl_core)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS xdl_core
    EXPORT xdl-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(XDL_BUILD_CLI)
    install(TARGETS xdl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT xdl-targets
    FILE xdl-targets.cmake
    NAMESPACE xdl::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xdl
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/xdl-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/xdl-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xdl
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/xdl-config-version.cmake
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/xdl-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/xdl-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xdl
)

# ---------------------------------------------------------------------------
# CPack (deb / rpm / tgz)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME        "xdl")
set(CPACK_PACKAGE_VERSION     "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION "eXtensible Data Layer - embedded storage engine")
set(CPACK_PACKAGE_CONTACT     "developer@example.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_HOMEPAGE_URL  "https://github.com/user/xdl")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Developer")
set(CPACK_DEBIAN_PACKAGE_DEPENDS     "libstdc++6")
set(CPACK_DEBIAN_PACKAGE_SECTION     "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS   ON)

set(CPACK_RPM_PACKAGE_LICENSE   "MIT")
set(CPACK_RPM_PACKAGE_GROUP     "Development/Libraries")
set(CPACK_RPM_PACKAGE_URL       "https://github.com/user/xdl")

include(CPack)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "XDL ${PROJECT_VERSION}")
message(STATUS "  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  CLI         : ${XDL_BUILD_CLI}")
message(STATUS "  Tests       : ${XDL_BUILD_TESTS}")
message(STATUS "  Bench       : ${XDL_BUILD_BENCH}")
message(STATUS "  Static link : ${XDL_STATIC_LINK}")
message(STATUS "  Install     : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
