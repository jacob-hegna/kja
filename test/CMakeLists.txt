add_executable(
   kja_test 

    server.cpp
    test.capnp.c++
)
target_link_libraries(kja_test PRIVATE GTest::gtest_main)
target_link_libraries(kja_test PRIVATE CapnProto::kj CapnProto::capnp CapnProto::capnpc CapnProto::capnp-rpc)
add_test(kja_test kja_test)
