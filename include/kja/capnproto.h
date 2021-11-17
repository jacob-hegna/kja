#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.capnp.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/threadlocal.h>

#include "async-io.h"

namespace kja {

class DummyFilter : public kj::LowLevelAsyncIoProvider::NetworkFilter {
public:
  bool shouldAllow(const struct sockaddr *addr, uint addrlen) override {
    return true;
  }
};

static DummyFilter DUMMY_FILTER;

class KjAsioContext {
public:
  KjAsioContext(boost::asio::io_context *ioc)
      : ioc_{ioc}, llprovider_{ioc}, provider_{
                                         kj::newAsyncIoProvider(llprovider_)} {}

  auto ioc() { return ioc_; }

  kj::LowLevelAsyncIoProvider &get_ll_provider() { return llprovider_; }
  kj::AsyncIoProvider &get_provider() { return *provider_; }

  auto &wait() { return llprovider_.wait(); }

private:
  boost::asio::io_context *ioc_;
  AsioLowLevelAsyncIoProvider llprovider_;
  kj::Own<kj::AsyncIoProvider> provider_;
};

struct EzRpcServer final : public capnp::SturdyRefRestorer<capnp::AnyPointer>,
                           public kj::TaskSet::ErrorHandler {
public:
  EzRpcServer(KjAsioContext *ctx, capnp::Capability::Client mainInterface,
              kj::StringPtr bindAddress, uint defaultPort,
              capnp::ReaderOptions readerOpts)
      : ctx_{ctx}, mainInterface(kj::mv(mainInterface)), portPromise(nullptr),
        tasks(*this) {
    auto paf = kj::newPromiseAndFulfiller<uint>();
    portPromise = paf.promise.fork();

    tasks.add(ctx_->get_provider()
                  .getNetwork()
                  .parseAddress(bindAddress, defaultPort)
                  .then(kj::mvCapture(
                      paf.fulfiller,
                      [this, readerOpts](
                          kj::Own<kj::PromiseFulfiller<uint>> &&portFulfiller,
                          kj::Own<kj::NetworkAddress> &&addr) {
                        auto listener = addr->listen();
                        portFulfiller->fulfill(listener->getPort());
                        acceptLoop(kj::mv(listener), readerOpts);
                      })));
  }

  EzRpcServer(KjAsioContext *ctx, capnp::Capability::Client mainInterface,
              int socketFd, uint port, capnp::ReaderOptions readerOpts)
      : ctx_{ctx}, mainInterface(kj::mv(mainInterface)),
        portPromise(kj::Promise<uint>(port).fork()), tasks(*this) {
    acceptLoop(
        ctx_->get_ll_provider().wrapListenSocketFd(socketFd, DUMMY_FILTER),
        readerOpts);
  }

  virtual ~EzRpcServer() {}

  void acceptLoop(kj::Own<kj::ConnectionReceiver> &&listener,
                  capnp::ReaderOptions readerOpts) {
    auto ptr = listener.get();
    tasks.add(ptr->accept().then(kj::mvCapture(
        kj::mv(listener),
        [this, readerOpts](kj::Own<kj::ConnectionReceiver> &&listener,
                           kj::Own<kj::AsyncIoStream> &&connection) {
          acceptLoop(kj::mv(listener), readerOpts);

          auto server =
              kj::heap<ServerContext>(kj::mv(connection), *this, readerOpts);

          // Arrange to destroy the server context when all references are gone,
          // or when the EzRpcServer is destroyed (which will destroy the
          // TaskSet).
          tasks.add(
              server->network.onDisconnect().attach(kj::mv(server)).then([]() {
                // fmt::print("\n DONE\n");
              }));
        })));
  }

  capnp::Capability::Client
  restore(capnp::AnyPointer::Reader objectId) override {
    if (objectId.isNull()) {
      return mainInterface;
    } else {
      std::terminate();
    }
  }

  void taskFailed(kj::Exception &&exception) override {
    kj::throwFatalException(kj::mv(exception));
  }

private:
  KjAsioContext *ctx_;
  capnp::Capability::Client mainInterface;

  // kj::AsyncIoContext ctx;

  kj::ForkedPromise<uint> portPromise;

  kj::TaskSet tasks;

  struct ServerContext {
    kj::Own<kj::AsyncIoStream> stream;
    capnp::TwoPartyVatNetwork network;
    capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpcSystem;

    ServerContext(kj::Own<kj::AsyncIoStream> &&stream,
                  SturdyRefRestorer<capnp::AnyPointer> &restorer,
                  capnp::ReaderOptions readerOpts)
        : stream(kj::mv(stream)),
          network(*this->stream, capnp::rpc::twoparty::Side::SERVER,
                  readerOpts),
          rpcSystem(capnp::makeRpcServer(network, restorer)) {}
  };
};

kj::Promise<kj::Own<kj::AsyncIoStream>>
connectAttach(kj::Own<kj::NetworkAddress> &&addr) {
  return addr->connect().attach(kj::mv(addr));
}

struct EzRpcClient {
private:
  KjAsioContext *ctx_;

  kj::ForkedPromise<void> setupPromise;

  struct ClientContext {
    kj::Own<kj::AsyncIoStream> stream;
    capnp::TwoPartyVatNetwork network;
    capnp::RpcSystem<capnp::rpc::twoparty::VatId> rpcSystem;

    ClientContext(kj::Own<kj::AsyncIoStream> &&stream,
                  capnp::ReaderOptions readerOpts)
        : stream(kj::mv(stream)),
          network(*this->stream, capnp::rpc::twoparty::Side::CLIENT,
                  readerOpts),
          rpcSystem(makeRpcClient(network)) {}

    capnp::Capability::Client getMain() {
      capnp::word scratch[4];
      memset(scratch, 0, sizeof(scratch));
      capnp::MallocMessageBuilder message(scratch);
      auto hostId = message.getRoot<capnp::rpc::twoparty::VatId>();
      hostId.setSide(capnp::rpc::twoparty::Side::SERVER);
      return rpcSystem.bootstrap(hostId);
    }

    capnp::Capability::Client restore(kj::StringPtr name) {
      capnp::word scratch[64];
      memset(scratch, 0, sizeof(scratch));
      capnp::MallocMessageBuilder message(scratch);

      auto hostIdOrphan =
          message.getOrphanage().newOrphan<capnp::rpc::twoparty::VatId>();
      auto hostId = hostIdOrphan.get();
      hostId.setSide(capnp::rpc::twoparty::Side::SERVER);

      auto objectId = message.getRoot<capnp::AnyPointer>();
      objectId.setAs<capnp::Text>(name);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
      return rpcSystem.restore(hostId, objectId);
#pragma GCC diagnostic pop
    }
  };

  kj::Maybe<kj::Own<ClientContext>> clientContext;

public:
  EzRpcClient(KjAsioContext *ctx, kj::StringPtr serverAddress, uint defaultPort,
              capnp::ReaderOptions readerOpts)
      : ctx_{ctx},
        setupPromise(
            ctx_->get_provider()
                .getNetwork()
                .parseAddress(serverAddress, defaultPort)
                .then([](kj::Own<kj::NetworkAddress> &&addr) {
                  return connectAttach(kj::mv(addr));
                })
                .then([this, readerOpts](kj::Own<kj::AsyncIoStream> &&stream) {
                  clientContext =
                      kj::heap<ClientContext>(kj::mv(stream), readerOpts);
                })
                .fork()) {}

  EzRpcClient(KjAsioContext *ctx, int socketFd, capnp::ReaderOptions readerOpts)
      : ctx_(ctx), setupPromise(kj::Promise<void>(kj::READY_NOW).fork()),
        clientContext(kj::heap<ClientContext>(
            ctx_->get_ll_provider().wrapSocketFd(socketFd), readerOpts)) {}

  template <class T> typename T::Client get_main() {
    KJ_IF_MAYBE(client, clientContext) {
      return client->get()->getMain().castAs<T>();
    }
    else {
      return setupPromise.addBranch().then([this]() -> typename T::Client {
        return KJ_ASSERT_NONNULL(clientContext)->getMain().castAs<T>();
      });
    }
  }
};

} // namespace kja
