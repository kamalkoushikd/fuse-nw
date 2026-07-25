cmake_minimum_required(VERSION 3.16)

project(fuse
    VERSION 0.1.0
    DESCRIPTION "Fuse: an experimental low-latency transport protocol"
    HOMEPAGE_URL "https://github.com/kamalkoushikd/fuse-nw"
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
option(FUSE_BUILD_BENCH "Build the fuse benchmark harness" ${FUSE_IS_TOP_LEVEL})
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

# --- fuse_proto: the staged datagram transport (Fuse work plan) ------------
#
# This is the protocol described in the staged build plan: a custom
# datagram block layer with per-stream registries, bitmask loss tracking,
# and (in later stages) worker threads and per-stream congestion control.
# It is intentionally a separate target from the `fuse` C library above and
# does NOT link wolfSSL — encryption is an opt-in later stage (DTLS/PSK),
# not a dependency of the core data plane.

add_library(fuse_proto
    src/proto/block.cpp
    src/proto/aux.cpp
    src/proto/registry.cpp
    src/proto/receiver.cpp
    src/proto/udp.cpp
    src/proto/setup.cpp
    src/proto/worker.cpp
    src/proto/reassembly.cpp
    src/proto/topology.cpp
    src/proto/dtls.cpp
    src/proto/orchestrator.cpp
    src/proto/session_crypto.cpp
    src/proto/transfer.cpp
    src/proto/sdk.cpp
)
add_library(fuse::proto ALIAS fuse_proto)

# Stage 7: optional DTLS/PSK encryption. Compiled in when the wolfSSL crypto
# backend is enabled; encryption still defaults to OFF at runtime
# (encryption_required=false). Without it, requiring encryption fails closed
# rather than silently running in plaintext.
if(FUSE_WITH_CRYPTO)
    target_compile_definitions(fuse_proto PUBLIC FUSE_PROTO_WITH_DTLS=1)
    target_link_libraries(fuse_proto PRIVATE wolfssl::wolfssl)
else()
    target_compile_definitions(fuse_proto PUBLIC FUSE_PROTO_WITH_DTLS=0)
endif()

find_package(Threads REQUIRED)

# Optional NUMA binding for topology-aware placement (Stage 6). Compiled in
# only when libnuma's development headers are present; otherwise the NUMA
# hook degrades to a documented no-op and CPU pinning still works.
find_path(FUSE_NUMA_INCLUDE_DIR numa.h)
find_library(FUSE_NUMA_LIBRARY numa)
if(FUSE_NUMA_INCLUDE_DIR AND FUSE_NUMA_LIBRARY)
    message(STATUS "fuse: libnuma found, enabling NUMA-aware placement")
    target_compile_definitions(fuse_proto PRIVATE FUSE_HAVE_NUMA=1)
    target_include_directories(fuse_proto PRIVATE ${FUSE_NUMA_INCLUDE_DIR})
    target_link_libraries(fuse_proto PRIVATE ${FUSE_NUMA_LIBRARY})
else()
    message(STATUS "fuse: libnuma not found, NUMA hook will be a no-op (CPU pinning still active)")
    target_compile_definitions(fuse_proto PRIVATE FUSE_HAVE_NUMA=0)
endif()

target_include_directories(fuse_proto
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # version.h is generated, so the build tree needs the binary dir too.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Worker threads (Stage 3) use std::thread; Threads is part of the public
# link interface so downstream consumers of fuse::proto link pthread too.
target_link_libraries(fuse_proto PUBLIC Threads::Threads)

# NOTE: unlike the C `fuse` library, fuse_proto does not yet use an export
# header, so its public symbols must stay visible (default visibility) for
# the shared library to export them. Revisit with generate_export_header
# once the Stage 1+ API surface settles.
set_target_properties(fuse_proto PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    # Without this the target exports as `fuse::fuse_proto`, since the
    # `fuse::proto` ALIAS above exists only in the build tree. Downstream
    # users would then have to link a different name than the one used
    # in-tree and in the docs.
    EXPORT_NAME proto
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fuse_proto PRIVATE -Wall -Wextra)
    if(FUSE_WARNINGS_AS_ERRORS)
        target_compile_options(fuse_proto PRIVATE -Werror)
    endif()
endif()

# --- Installation ---------------------------------------------------------

# Let the installed libraries find their siblings (notably libwolfssl, which
# is installed alongside them) from wherever the package ends up.
#
# This matters more than it looks: modern linkers emit RUNPATH rather than
# RPATH, and the loader consults RUNPATH only for an object's *direct*
# dependencies. So an executable's rpath does NOT help libfuse_proto.so find
# libwolfssl.so — without $ORIGIN here, anything installed outside the system
# library path fails at run time with a missing-library error, even though it
# linked cleanly.
set_target_properties(fuse fuse_proto PROPERTIES
    INSTALL_RPATH "$ORIGIN"
)

install(TARGETS fuse fuse_proto
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

# fuse.pc's `prefix=` line must reflect the prefix actually in effect
# at install time (which `cmake --install --prefix` and CPack's
# packaging-time re-rooting both override), not the prefix seen at
# configure time. configure_file() alone would bake in the latter, so
# the substitution is deferred into an install(CODE) script, where
# CMAKE_INSTALL_PREFIX is re-evaluated as the real target of this
# particular install.
if(FUSE_WITH_CRYPTO)
    set(FUSE_PC_REQUIRES_PRIVATE "wolfssl")
else()
    set(FUSE_PC_REQUIRES_PRIVATE "")
endif()

install(CODE "
    file(READ \"${CMAKE_CURRENT_SOURCE_DIR}/cmake/fuse.pc.in\" _fuse_pc_content)
    string(REPLACE \"@CMAKE_INSTALL_PREFIX@\" \"\${CMAKE_INSTALL_PREFIX}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@CMAKE_INSTALL_LIBDIR@\" \"${CMAKE_INSTALL_LIBDIR}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@CMAKE_INSTALL_INCLUDEDIR@\" \"${CMAKE_INSTALL_INCLUDEDIR}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@PROJECT_DESCRIPTION@\" \"${PROJECT_DESCRIPTION}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@PROJECT_VERSION@\" \"${PROJECT_VERSION}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@PROJECT_HOMEPAGE_URL@\" \"${PROJECT_HOMEPAGE_URL}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    string(REPLACE \"@FUSE_PC_REQUIRES_PRIVATE@\" \"${FUSE_PC_REQUIRES_PRIVATE}\" _fuse_pc_content \"\${_fuse_pc_content}\")
    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/fuse.pc\" \"\${_fuse_pc_content}\")
")
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

if(FUSE_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# --- Packaging (CPack) ------------------------------------------------------
#
# Produces a single combined package per generator (library, headers,
# CMake/pkg-config files, and - since FUSE_FETCH_WOLFSSL builds and
# installs its own wolfSSL alongside fuse - that too). This is
# deliberately not split into separate runtime/-dev packages yet: doing
# that properly would require retrofitting COMPONENT tags onto
# wolfSSL's own install() rules, which fuse doesn't control. A single
# "install everything needed to build against and run fuse" package is
# the standard approach for a pre-1.0 library distributed outside of a
# distro's own repositories.

if(FUSE_IS_TOP_LEVEL)
    set(CPACK_PACKAGE_NAME "fuse")
    set(CPACK_PACKAGE_VENDOR "The Fuse Project")
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_CONTACT "The Fuse Project <kamalkoushik24@gmail.com>")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_STRIP_FILES ON)

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
    set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)

    set(CPACK_GENERATOR "TGZ")
    find_program(FUSE_DPKG_DEB_EXECUTABLE dpkg-deb)
    find_program(FUSE_RPMBUILD_EXECUTABLE rpmbuild)
    if(FUSE_DPKG_DEB_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
    endif()
    if(FUSE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()

    include(CPack)
endif()
