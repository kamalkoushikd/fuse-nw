add_executable(fuse_echo_server echo_server.c)
target_link_libraries(fuse_echo_server PRIVATE fuse::fuse)

add_executable(fuse_echo_client echo_client.cpp)
target_link_libraries(fuse_echo_client PRIVATE fuse::fuse)
