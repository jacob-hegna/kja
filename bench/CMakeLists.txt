add_executable(
    kja_bench

    server.cpp
    bench.capnp.c++
)
target_link_libraries(kja_bench PRIVATE benchmark::benchmark benchmark::benchmark_main)
target_link_libraries(kja_bench PRIVATE CapnProto::kj CapnProto::capnp CapnProto::capnpc CapnProto::capnp-rpc)

