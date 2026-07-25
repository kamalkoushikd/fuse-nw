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

        # AES-NI. wolfSSL's CMake build has no option for this (it is an
        # autotools-only flag), so without wiring it up by hand wolfSSL
        # silently uses its portable C AES: measured here at 151 MB/s versus
        # multiple GB/s with hardware instructions. For a transport that
        # encrypts every byte, that difference is the whole performance story,
        # so it is worth doing explicitly rather than leaving to a default.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            include(CheckCSourceCompiles)
            set(CMAKE_REQUIRED_FLAGS "-maes -mpclmul -msse4.1")
            check_c_source_compiles("
                #include <wmmintrin.h>
                int main(void) {
                    __m128i a = _mm_setzero_si128();
                    a = _mm_aesenc_si128(a, a);
                    a = _mm_clmulepi64_si128(a, a, 0);
                    return _mm_cvtsi128_si32(a);
                }" FUSE_HAVE_AESNI_INTRINSICS)
            unset(CMAKE_REQUIRED_FLAGS)

            # AES-NI needs BOTH assembly units: aes_asm.S has the block cipher,
            # aes_gcm_asm.S has the GCM/GHASH routines. Adding only the first
            # links but leaves AES_GCM_*_aesni undefined.
            if(FUSE_HAVE_AESNI_INTRINSICS AND
               EXISTS "${wolfssl_SOURCE_DIR}/wolfcrypt/src/aes_asm.S" AND
               EXISTS "${wolfssl_SOURCE_DIR}/wolfcrypt/src/aes_gcm_asm.S")
                message(STATUS "fuse: enabling wolfSSL AES-NI (hardware AES-GCM)")
                target_sources(wolfssl PRIVATE
                    "${wolfssl_SOURCE_DIR}/wolfcrypt/src/aes_asm.S"
                    "${wolfssl_SOURCE_DIR}/wolfcrypt/src/aes_gcm_asm.S")
                target_compile_definitions(wolfssl PRIVATE WOLFSSL_AESNI)
                target_compile_options(wolfssl PRIVATE -maes -mpclmul -msse4.1)
            else()
                message(STATUS "fuse: AES-NI unavailable, wolfSSL will use software AES")
            endif()
        endif()
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
