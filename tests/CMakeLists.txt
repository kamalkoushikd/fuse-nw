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

# Stage 1 datagram-protocol tests (fuse_proto).
add_executable(fuse_proto_tests
    proto/test_wire.cpp
    proto/test_block.cpp
    proto/test_aux.cpp
    proto/test_registry.cpp
    proto/test_receiver.cpp
    proto/test_stage1_loopback.cpp
    proto/test_setup.cpp
    proto/test_worker.cpp
    proto/test_flags.cpp
    proto/test_congestion.cpp
    proto/test_topology.cpp
    proto/test_dtls.cpp
    proto/test_orchestrator.cpp
)

target_link_libraries(fuse_proto_tests PRIVATE fuse::proto GTest::gtest_main)

gtest_discover_tests(fuse_proto_tests)
