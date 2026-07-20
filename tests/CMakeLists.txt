include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(fuse_tests
    test_varint.cpp
    test_packet.cpp
    test_crypto.cpp
)

target_link_libraries(fuse_tests PRIVATE fuse::fuse GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(fuse_tests)
