add_executable(fuse_bench fuse_bench.cpp)
target_link_libraries(fuse_bench PRIVATE fuse::proto)

# Reliable file transfer, used for the head-to-head comparison against QUIC.
add_executable(fuse_filebench fuse_filebench.cpp)
target_link_libraries(fuse_filebench PRIVATE fuse::proto)

# Latency + goodput benchmark: small-message latency under bulk saturation.
add_executable(fuse_latbench fuse_latbench.cpp)
target_link_libraries(fuse_latbench PRIVATE fuse::proto)

# Userspace network emulator (tc netem substitute): loss/delay/jitter/reorder.
add_executable(fuse_netem fuse_netem.cpp)
target_link_libraries(fuse_netem PRIVATE Threads::Threads)
