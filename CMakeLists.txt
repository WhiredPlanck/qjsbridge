cmake_minimum_required(VERSION 3.16)
project(qjsbridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Header-only interface library ─────────────────────────────────────────────
add_library(qjsbridge INTERFACE)
add_library(qjsbridge::qjsbridge ALIAS qjsbridge)

target_include_directories(qjsbridge INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qjsbridge INTERFACE cxx_std_17)

# ── QuickJS detection (original or QuickJS-ng) ────────────────────────────────
find_package(PkgConfig QUIET)

set(QUICKJS_FOUND FALSE)

# 1. Try pkg-config first
if(PkgConfig_FOUND)
    pkg_check_modules(QUICKJS QUIET quickjs)
    if(QUICKJS_FOUND)
        message(STATUS "Found QuickJS via pkg-config")
    endif()
endif()

# 2. Manual search using well-known install layout
if(NOT QUICKJS_FOUND)
    find_path(QUICKJS_INCLUDE_DIR
        NAMES quickjs.h
        PATHS /usr/include/quickjs /usr/local/include/quickjs
        NO_DEFAULT_PATH)
    find_library(QUICKJS_LIBRARY
        NAMES quickjs
        PATHS /usr/lib/quickjs /usr/local/lib/quickjs /usr/lib /usr/local/lib
        NO_DEFAULT_PATH)
    if(QUICKJS_INCLUDE_DIR AND QUICKJS_LIBRARY)
        set(QUICKJS_FOUND TRUE)
        set(QUICKJS_INCLUDE_DIRS "${QUICKJS_INCLUDE_DIR}")
        set(QUICKJS_LIBRARIES    "${QUICKJS_LIBRARY}")
        message(STATUS "Found QuickJS: ${QUICKJS_LIBRARY}")
    endif()
endif()

# 3. Last resort: standard path search
if(NOT QUICKJS_FOUND)
    find_path(QUICKJS_INCLUDE_DIR  NAMES quickjs.h    PATH_SUFFIXES quickjs)
    find_library(QUICKJS_LIBRARY   NAMES quickjs)
    if(QUICKJS_INCLUDE_DIR AND QUICKJS_LIBRARY)
        set(QUICKJS_FOUND TRUE)
        set(QUICKJS_INCLUDE_DIRS "${QUICKJS_INCLUDE_DIR}")
        set(QUICKJS_LIBRARIES    "${QUICKJS_LIBRARY}")
        message(STATUS "Found QuickJS: ${QUICKJS_LIBRARY}")
    endif()
endif()

# ── Combined interface target that also links QuickJS ─────────────────────────
if(QUICKJS_FOUND)
    add_library(qjsbridge_with_quickjs INTERFACE)
    target_link_libraries(qjsbridge_with_quickjs INTERFACE qjsbridge)
    if(QUICKJS_INCLUDE_DIRS)
        target_include_directories(qjsbridge_with_quickjs INTERFACE
            ${QUICKJS_INCLUDE_DIRS})
    endif()
    target_link_libraries(qjsbridge_with_quickjs INTERFACE ${QUICKJS_LIBRARIES})
    if(QUICKJS_CFLAGS_OTHER)
        target_compile_options(qjsbridge_with_quickjs INTERFACE
            ${QUICKJS_CFLAGS_OTHER})
    endif()
    # Detect QuickJS-ng by the presence of version macro – adjust as needed
    if(DEFINED QJSBRIDGE_QUICKJS_NG OR "$ENV{QJSBRIDGE_QUICKJS_NG}" STREQUAL "1")
        target_compile_definitions(qjsbridge_with_quickjs INTERFACE
            QJSBRIDGE_QUICKJS_NG)
    endif()
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(QJSBRIDGE_BUILD_TESTS    "Build unit tests"   ON)
option(QJSBRIDGE_BUILD_EXAMPLES "Build examples"     ON)

if(QUICKJS_FOUND AND QJSBRIDGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(QUICKJS_FOUND AND QJSBRIDGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NOT QUICKJS_FOUND)
    message(WARNING
        "QuickJS not found – tests and examples will not be built.\n"
        "Install QuickJS-ng (https://github.com/quickjs-ng/quickjs) and re-run cmake.")
endif()

# ── Installation ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS qjsbridge EXPORT qjsbridgeTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT qjsbridgeTargets
    FILE qjsbridgeTargets.cmake
    NAMESPACE qjsbridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qjsbridge)
