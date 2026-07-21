add_executable(fuse_bench fuse_bench.cpp)
target_link_libraries(fuse_bench PRIVATE fuse::proto)

# Reliable file transfer, used for the head-to-head comparison against QUIC.
add_executable(fuse_filebench fuse_filebench.cpp)
target_link_libraries(fuse_filebench PRIVATE fuse::proto)
