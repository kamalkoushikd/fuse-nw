cmake_minimum_required(VERSION 3.16)

project(fuse
    VERSION 0.1.0
    DESCRIPTION "Fuse: an experimental low-latency transport protocol"
    HOMEPAGE_URL "https://github.com/CHANGEME/fuse"
    LANGUAGES C CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FUSE_IS_TOP_LEVEL ON)
else()
    set(FUSE_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(BUILD_SHARED_LIBS "Build fuse as a shared library" ON)
option(FUSE_BUILD_TESTS "Build the fuse test suite" ${FUSE_IS_TOP_LEVEL})
option(FUSE_BUILD_EXAMPLES "Build the fuse example programs" ${FUSE_IS_TOP_LEVEL})
option(FUSE_WITH_CRYPTO "Enable the wolfSSL-backed handshake/crypto module" ON)
option(FUSE_FETCH_WOLFSSL "Fetch and build wolfSSL from source if it isn't found on the system" ON)
option(FUSE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "fuse targets Linux; other platforms are untested")
endif()

if(FUSE_WITH_CRYPTO)
    include(FuseWolfSSL)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/fuse/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/fuse/version.h"
    @ONLY
)

add_library(fuse
    src/varint.c
    src/packet.c
    src/socket.c
    src/connection.c
    src/crypto.c
)
add_library(fuse::fuse ALIAS fuse)

generate_export_header(fuse
    BASE_NAME fuse
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/fuse/export.h"
)

target_include_directories(fuse
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(fuse PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fuse PRIVATE -Wall -Wextra)
    if(FUSE_WARNINGS_AS_ERRORS)
        target_compile_options(fuse PRIVATE -Werror)
    endif()
endif()

if(FUSE_WITH_CRYPTO)
    target_compile_definitions(fuse PUBLIC FUSE_WITH_CRYPTO=1)
    target_link_libraries(fuse PRIVATE wolfssl::wolfssl)
else()
    target_compile_definitions(fuse PUBLIC FUSE_WITH_CRYPTO=0)
endif()

# --- Installation ---------------------------------------------------------

install(TARGETS fuse
    EXPORT fuse-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/fuse
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/include/fuse/version.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/fuse/export.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fuse
)

install(EXPORT fuse-targets
    FILE fuse-targets.cmake
    NAMESPACE fuse::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fuse
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuseConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fuseConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fuse
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/fuseConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/fuseConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/fuseConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fuse
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuse.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fuse.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fuse.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# --- Subprojects -----------------------------------------------------------

if(FUSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FUSE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
