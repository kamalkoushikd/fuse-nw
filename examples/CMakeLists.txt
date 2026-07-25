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

# SDK examples: a concurrent echo server/client using the socket-style API.
# These are plain C, exactly as an external developer would write them.
add_executable(fuse_echo_sdk_server sdk/echo_server.c)
target_link_libraries(fuse_echo_sdk_server PRIVATE fuse::proto)

add_executable(fuse_echo_sdk_client sdk/echo_client.c)
target_link_libraries(fuse_echo_sdk_client PRIVATE fuse::proto)
