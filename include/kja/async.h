/*
 * Defines the interaction between asio and kj async programming models.
 * Namely, it defines an event port for a boost::asio::io_context to run a
 * kj::EventLoop.
 */

#pragma once

#include <boost/asio/io_context.hpp>
#include <kj/async-io.h>

namespace kja {

class AsioEventPort : public kj::EventPort {
public:
  AsioEventPort(boost::asio::io_context *ioc) : ioc_{ioc}, kj_loop_{*this} {}

  virtual ~AsioEventPort() {}

  kj::EventLoop &getKjLoop() { return kj_loop_; }
  boost::asio::io_context *ioc() { return ioc_; }

  bool wait() override {
    ioc_->run_one();
    return false;
  }

  bool poll() override {
    ioc_->poll();
    return false;
  }

  void setRunnable(bool runnable) override {
    // TODO: this seems too simple of an implementation
    if (runnable) {
      ioc_->post([this]() { kj_loop_.run(); });
    }
  }

private:
  boost::asio::io_context *ioc_;
  kj::EventLoop kj_loop_;
};

} // namespace kja
