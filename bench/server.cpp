#include <benchmark/benchmark.h>
#include <capnp/ez-rpc.h>
#include <kja/kja.h>

#include "bench.capnp.h"

class TestInterfaceImpl final : public TestInterface::Server {
public:
  kj::Promise<void> echo(EchoContext ctx) override {
    ctx.getResults().setRes(ctx.getParams().getReq());
    return kj::READY_NOW;
  }
};

static void BM_AsioServer(benchmark::State &state) {
  boost::asio::io_context ioc;
  kja::KjAsioContext ctx(&ioc);
  kja::EzAsioServer server(&ctx, kj::heap<TestInterfaceImpl>(), "*", 5923,
                           capnp::ReaderOptions());
  kja::AsioRpcClient client(&ctx, "[::1]:5923", 0, capnp::ReaderOptions());

  auto cap = client.get_main<TestInterface>();
  for (auto _ : state) {
    auto req = cap.echoRequest();
    req.setReq("asdf");

    std::string res_str;
    req.send()
        .then([&](auto resp) { res_str = resp.getRes().cStr(); })
        .wait(ctx.wait());
  }
}
BENCHMARK(BM_AsioServer);

static void BM_KjServer(benchmark::State &state) {
  capnp::EzRpcServer server(kj::heap<TestInterfaceImpl>(), "*", 5923,
                            capnp::ReaderOptions());
  capnp::EzRpcClient client("[::1]:5923", 0, capnp::ReaderOptions());
  auto cap = client.getMain<TestInterface>();

  for (auto _ : state) {
    auto req = cap.echoRequest();
    req.setReq("asdf");

    std::string res_str;
    req.send()
        .then([&](auto resp) { res_str = resp.getRes().cStr(); })
        .wait(client.getWaitScope());
  }
}
BENCHMARK(BM_KjServer);
