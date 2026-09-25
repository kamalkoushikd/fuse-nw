add_executable(fuse_echo_server echo_server.c)
target_link_libraries(fuse_echo_server PRIVATE fuse::fuse)

add_executable(fuse_echo_client echo_client.cpp)
target_link_libraries(fuse_echo_client PRIVATE fuse::fuse)

# Stage 1 datagram-protocol loopback demos.
add_executable(fuse_stage1_receiver stage1_receiver.cpp)
target_link_libraries(fuse_stage1_receiver PRIVATE fuse::proto)

add_executable(fuse_stage1_sender stage1_sender.cpp)
target_link_libraries(fuse_stage1_sender PRIVATE fuse::proto)

# Quickstart: the smallest complete send/receive pair using the public API.
add_executable(fuse_quickstart_recv quickstart_recv.cpp)
target_link_libraries(fuse_quickstart_recv PRIVATE fuse::proto)

add_executable(fuse_quickstart_send quickstart_send.cpp)
target_link_libraries(fuse_quickstart_send PRIVATE fuse::proto)

# The two quickstart tools are the only examples meant to be installed and
# run standalone (everything else here is a development/demo build, run
# from the build tree) — matches what scripts/make_release.sh already
# curates into a release tarball's bin/, and what docs/USAGE.md's smoke
# test and scripts/install.sh's printed instructions already assume exists
# at $PREFIX/bin after an install.
#
# INSTALL_RPATH: libfuse_proto.so is these executables' own direct
# dependency (unlike the libwolfssl case in the top-level CMakeLists.txt
# comment, RUNPATH does apply here), so without this an install to a
# non-system prefix — bin/ separate from lib/ — links fine but fails at
# run time with a missing-library error.
set_target_properties(fuse_quickstart_recv fuse_quickstart_send PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS fuse_quickstart_recv fuse_quickstart_send
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# SDK examples: a concurrent echo server/client using the socket-style API.
# These are plain C, exactly as an external developer would write them.
add_executable(fuse_echo_sdk_server sdk/echo_server.c)
target_link_libraries(fuse_echo_sdk_server PRIVATE fuse::proto)

add_executable(fuse_echo_sdk_client sdk/echo_client.c)
target_link_libraries(fuse_echo_sdk_client PRIVATE fuse::proto)
