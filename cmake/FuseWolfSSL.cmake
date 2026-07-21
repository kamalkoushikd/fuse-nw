# Locates a wolfssl::wolfssl target for fuse's crypto module.
#
# Resolution order:
#   1. An already-installed wolfSSL exposing a CMake package config
#      (wolfssl-config.cmake), which upstream installs alongside the
#      library and which defines the `wolfssl::wolfssl` target.
#   2. If FUSE_FETCH_WOLFSSL is enabled, fetch and build wolfSSL from
#      source with the options fuse needs (TLS 1.3 + QUIC record layer
#      support, which also pulls in HKDF).
#
# Either path leaves a `wolfssl::wolfssl` target available to the rest
# of the build.

find_package(wolfssl CONFIG QUIET)

if(TARGET wolfssl::wolfssl)
    message(STATUS "fuse: using system wolfSSL (found via CMake config)")
elseif(FUSE_FETCH_WOLFSSL)
    message(STATUS "fuse: wolfSSL not found on system, fetching from source")
    include(FetchContent)

    set(WOLFSSL_TLS13 yes CACHE STRING "" FORCE)
    set(WOLFSSL_QUIC yes CACHE STRING "" FORCE)
    set(WOLFSSL_HKDF yes CACHE STRING "" FORCE)
    set(WOLFSSL_EXAMPLES no CACHE STRING "" FORCE)
    set(WOLFSSL_CRYPT_TESTS no CACHE STRING "" FORCE)

    # Stage 7 optional encryption rides on DTLS (the datagram-appropriate
    # counterpart to TLS) with pre-shared keys. WOLFSSL_PSK is not a declared
    # wolfSSL option but is read by its CMakeLists: leaving it unset makes
    # wolfSSL compile in -DNO_PSK, which would strip the PSK cipher suites.
    set(WOLFSSL_DTLS yes CACHE STRING "" FORCE)
    set(WOLFSSL_PSK yes CACHE STRING "" FORCE)

    FetchContent_Declare(wolfssl
        GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
        GIT_TAG v5.9.2-stable
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(wolfssl)

    if(TARGET wolfssl)
        # wolfSSL's own CMakeLists.txt hard-codes -Werror. On newer
        # glibc/GCC combinations this has tripped over implicit
        # declarations (e.g. gmtime_r without _DEFAULT_SOURCE) that are
        # upstream's problem, not fuse's; don't let fuse's build be
        # fragile against wolfSSL's warning strictness on toolchains
        # newer than whatever it was last tested against.
        target_compile_options(wolfssl PRIVATE -Wno-error)
        target_compile_definitions(wolfssl PRIVATE _DEFAULT_SOURCE)
    endif()

    if(NOT TARGET wolfssl::wolfssl)
        message(FATAL_ERROR
            "fuse: fetched wolfSSL but the expected `wolfssl::wolfssl` "
            "target was not defined; the pinned wolfSSL tag may have "
            "changed its CMake target layout")
    endif()
else()
    message(FATAL_ERROR
        "fuse: wolfSSL was not found on the system and FUSE_FETCH_WOLFSSL "
        "is OFF. Install wolfSSL (built with -DWOLFSSL_QUIC=yes "
        "-DWOLFSSL_TLS13=yes) or re-enable FUSE_FETCH_WOLFSSL.")
endif()
