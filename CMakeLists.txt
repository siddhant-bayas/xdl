cmake_minimum_required(VERSION 3.16)
project(xdl VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler settings
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings — strict but not pedantic-error so third-party headers compile fine
if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()
if (NOT MSVC)
    add_compile_options(-fsanitize=address,undefined)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Optional: system LZ4 (falls back to bundled impl if not found)
# ─────────────────────────────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 liblz4)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Core library
# ─────────────────────────────────────────────────────────────────────────────
add_library(xdl_core STATIC
    src/page.cpp
    src/compression.cpp
    src/storage.cpp
    src/cache.cpp
    src/index.cpp
    src/pager.cpp
    src/db.cpp
)

target_include_directories(xdl_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(LZ4_FOUND)
    target_compile_definitions(xdl_core PUBLIC XDL_HAVE_LZ4)
    target_include_directories(xdl_core PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(xdl_core PRIVATE ${LZ4_LIBRARIES})
    message(STATUS "XDL: using system LZ4 (${LZ4_VERSION})")
else()
    message(STATUS "XDL: using bundled LZ4 implementation")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# CLI executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(xdl src/main.cpp)
target_link_libraries(xdl PRIVATE xdl_core)

# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()
add_executable(xdl_tests tests/test_xdl.cpp)
target_link_libraries(xdl_tests PRIVATE xdl_core)
add_test(NAME xdl_unit_tests COMMAND xdl_tests)

# ─────────────────────────────────────────────────────────────────────────────
# Install rules
# ─────────────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS xdl xdl_core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "XDL ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")