#include <gtest/gtest.h>

#include <kja/kja.h>

#include "test.capnp.h"

class TestInterfaceImpl final : public TestInterface::Server {
public:
  kj::Promise<void> echo(EchoContext ctx) override {
    ctx.getResults().setRes(ctx.getParams().getReq());
    return kj::READY_NOW;
  }
};

TEST(CapnpServer, Echo) {
  boost::asio::io_context ioc;
  kja::KjAsioContext ctx(&ioc);
  kja::EzAsioServer server(&ctx, kj::heap<TestInterfaceImpl>(), "*", 5923,
                           capnp::ReaderOptions());

  kja::AsioRpcClient client(&ctx, "[::1]:5923", 0, capnp::ReaderOptions());
  auto cap = client.get_main<TestInterface>();
  auto req = cap.echoRequest();
  req.setReq("asdf");

  std::string res_str;
  req.send()
      .then([&](auto resp) { res_str = resp.getRes().cStr(); })
      .wait(ctx.wait());

  ASSERT_EQ(res_str, "asdf");
}
