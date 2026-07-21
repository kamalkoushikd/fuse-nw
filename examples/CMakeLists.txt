add_executable(fuse_echo_server echo_server.c)
target_link_libraries(fuse_echo_server PRIVATE fuse::fuse)

add_executable(fuse_echo_client echo_client.cpp)
target_link_libraries(fuse_echo_client PRIVATE fuse::fuse)

# Stage 1 datagram-protocol loopback demos.
add_executable(fuse_stage1_receiver stage1_receiver.cpp)
target_link_libraries(fuse_stage1_receiver PRIVATE fuse::proto)

add_executable(fuse_stage1_sender stage1_sender.cpp)
target_link_libraries(fuse_stage1_sender PRIVATE fuse::proto)
