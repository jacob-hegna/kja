/*
 * Implements the kj machinery to perform async io using asio.
 */

#pragma once

#include <iostream>
#include <sys/fcntl.h>

#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <kj/async-io.h>
#include <kj/debug.h>

#include "async.h"

namespace kja {

inline void setNonblocking(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFL));
  if ((flags & O_NONBLOCK) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
  }
}

inline void setCloseOnExec(int fd) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd, F_GETFD));
  if ((flags & FD_CLOEXEC) == 0) {
    KJ_SYSCALL(fcntl(fd, F_SETFD, flags | FD_CLOEXEC));
  }
}

inline int applyFlags(int fd, uint flags) {
  if (flags & kj::LowLevelAsyncIoProvider::ALREADY_NONBLOCK) {
    KJ_DREQUIRE(fcntl(fd, F_GETFL) & O_NONBLOCK,
                "You claimed you set NONBLOCK, but you didn't.");
  } else {
    setNonblocking(fd);
  }

  if (flags & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP) {
    if (flags & kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC) {
      KJ_DREQUIRE(fcntl(fd, F_GETFD) & FD_CLOEXEC,
                  "You claimed you set CLOEXEC, but you didn't.");
    } else {
      setCloseOnExec(fd);
    }
  }

  return fd;
}

class OwnedFd {
public:
  OwnedFd(boost::asio::io_context *ioc, int fd, uint flags)
      : ioc_(ioc), async_fd_(*ioc_, applyFlags(fd, flags)), flags_{flags} {}

  ~OwnedFd() noexcept(false) {
    async_fd_.cancel();
    // If we aren't supposed to own the file, release it here
    // if (!(flags_ & kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)) {
    //  async_fd_.release();
    //} else {
    //  ::close(async_fd_.native_handle());
    //}
  }

  kj::Promise<void> on_readable() {
    return on_event(boost::asio::posix::stream_descriptor::wait_read);
  }

  kj::Promise<void> on_writeable() {
    return on_event(boost::asio::posix::stream_descriptor::wait_write);
  }

protected:
  kj::Promise<void>
  on_event(boost::asio::posix::stream_descriptor::wait_type des) {
    auto paf = kj::newPromiseAndFulfiller<void>();
    async_fd_.async_wait(
        des, [ful = std::move(paf.fulfiller)](
                 boost::system::error_code ec) mutable { ful->fulfill(); });
    return std::move(paf.promise);
  }

  boost::asio::io_context *ioc_;
  boost::asio::posix::stream_descriptor async_fd_;
  uint flags_;
};

class AsioIoStream : public kj::AsyncIoStream, public OwnedFd {
public:
  AsioIoStream(boost::asio::io_context *ioc, int fd, uint flags = 0)
      : OwnedFd(ioc, fd, flags), ioc_(ioc) {}

  virtual ~AsioIoStream() {}

  kj::Promise<size_t> read(void *buffer, size_t minBytes,
                           size_t maxBytes) override {
    auto paf = kj::newPromiseAndFulfiller<size_t>();
    boost::asio::async_read(
        async_fd_, boost::asio::buffer(buffer, minBytes),
        [ful = std::move(paf.fulfiller)](boost::system::error_code ec,
                                         size_t bytes) mutable {
          ful->fulfill(std::move(bytes));
        });
    return std::move(paf.promise);
  }

  kj::Promise<size_t> tryRead(void *buffer, size_t minBytes,
                              size_t maxBytes) override {
    auto paf = kj::newPromiseAndFulfiller<size_t>();
    boost::asio::async_read(
        async_fd_, boost::asio::buffer(buffer, minBytes),
        [ful = std::move(paf.fulfiller)](boost::system::error_code ec,
                                         size_t bytes) mutable {
          ful->fulfill(std::move(bytes));
        });
    return std::move(paf.promise);
  }

  kj::Promise<void> write(const void *buffer, size_t size) override {
    auto paf = kj::newPromiseAndFulfiller<void>();
    boost::asio::async_write(async_fd_, boost::asio::buffer(buffer, size),
                             [ful = std::move(paf.fulfiller)](
                                 boost::system::error_code ec,
                                 size_t bytes) mutable { ful->fulfill(); });
    return std::move(paf.promise);
  }

  kj::Promise<void>
  write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    auto paf = kj::newPromiseAndFulfiller<void>();

    std::vector<boost::asio::const_buffer> buffers;
    for (const auto &piece : pieces) {
      buffers.emplace_back(
          boost::asio::const_buffer(piece.begin(), piece.size()));
    }

    boost::asio::async_write(async_fd_, buffers,
                             [ful = std::move(paf.fulfiller)](
                                 boost::system::error_code ec,
                                 size_t bytes) mutable { ful->fulfill(); });
    return std::move(paf.promise);
  }

  void shutdownWrite() override {}
  kj::Promise<void> whenWriteDisconnected() override { return kj::NEVER_DONE; }

private:
  boost::asio::io_context *ioc_;
};

class AsioConnectionReceiver final : public kj::ConnectionReceiver,
                                     public OwnedFd {
public:
  AsioConnectionReceiver(boost::asio::io_context *ioc, int fd, uint flags)
      : OwnedFd(ioc, fd, flags), ioc_{ioc}, fd_{fd} {}

  virtual ~AsioConnectionReceiver() {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    int newFd;

    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

#if __linux__
    newFd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    fcntl(fd_, F_SETFL, (fcntl(fd_, F_GETFL) | O_NONBLOCK));
    fcntl(fd_, F_SETFL, (fcntl(fd_, F_GETFL) | O_CLOEXEC));
    newFd = ::accept(fd_, reinterpret_cast<struct sockaddr *>(&addr), &addrlen);
#endif

    if (newFd >= 0) {
      int one = 1;
      ::setsockopt(newFd, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));

      return kj::Own<kj::AsyncIoStream>(kj::heap<AsioIoStream>(
          ioc_, newFd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP));
    } else {
      int error = errno;

      switch (error) {
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
        // Not ready yet.
        return on_readable().then([this]() { return accept(); });

      case EINTR:
      case ENETDOWN:
#ifdef EPROTO
      // EPROTO is not defined on OpenBSD.
      case EPROTO:
#endif
      case EHOSTDOWN:
      case EHOSTUNREACH:
      case ENETUNREACH:
      case ECONNABORTED:
      case ETIMEDOUT:
        // According to the Linux man page, accept() may report an error if the
        // accepted connection is already broken.  In this case, we really ought
        // to just ignore it and keep waiting.  But it's hard to say exactly
        // what errors are such network errors and which ones are permanent
        // errors.  We've made a guess here.
        return accept();

      default:
        KJ_FAIL_SYSCALL("accept", error);
      }
    }
    return nullptr;
  }

  uint getPort() override {
    socklen_t addrlen;
    union {
      struct sockaddr generic;
      struct sockaddr_in inet4;
      struct sockaddr_in6 inet6;
    } addr;
    addrlen = sizeof(addr);
    KJ_SYSCALL(::getsockname(fd_, &addr.generic, &addrlen));
    switch (addr.generic.sa_family) {
    case AF_INET:
      return ntohs(addr.inet4.sin_port);
    case AF_INET6:
      return ntohs(addr.inet6.sin6_port);
    default:
      return 0;
    }
  }

private:
  boost::asio::io_context *ioc_;
  int fd_;
};

class AsioLowLevelAsyncIoProvider final : public kj::LowLevelAsyncIoProvider {
public:
  AsioLowLevelAsyncIoProvider(boost::asio::io_context *ioc)
      : ioc_{ioc}, event_port_(ioc), wait_scope_(event_port_.getKjLoop()) {}

  virtual ~AsioLowLevelAsyncIoProvider() {}

  inline kj::WaitScope &getWaitScope() { return wait_scope_; }
  inline kj::EventPort &getEventPort() { return event_port_; }

  kj::Own<kj::AsyncInputStream> wrapInputFd(int fd, uint flags = 0) override {
    return kj::heap<AsioIoStream>(event_port_.ioc(), fd, flags);
  }
  kj::Own<kj::AsyncOutputStream> wrapOutputFd(int fd, uint flags = 0) override {
    return kj::heap<AsioIoStream>(event_port_.ioc(), fd, flags);
  }
  kj::Own<kj::AsyncIoStream> wrapSocketFd(int fd, uint flags = 0) override {
    return kj::heap<AsioIoStream>(event_port_.ioc(), fd, flags);
  }
  kj::Promise<kj::Own<kj::AsyncIoStream>>
  wrapConnectingSocketFd(int fd, const struct sockaddr *addr, uint addrlen,
                         uint flags = 0) override {

    // boost::asio::ip::tcp::socket soc(*ioc_);
    // soc.assign(boost::asio::ip::tcp::v4(), fd);

    auto result = kj::heap<AsioIoStream>(event_port_.ioc(), fd, flags);

    // char clienthost[NI_MAXHOST];
    // char clientport[NI_MAXSERV];
    // int r = getnameinfo(addr, sizeof(*addr), clienthost, sizeof(clienthost),
    //                    clientport, sizeof(clientport),
    //                    NI_NUMERICHOST | NI_NUMERICSERV);
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
    char ipAddress[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(ipv4->sin_addr), ipAddress, INET_ADDRSTRLEN);

    //// Need to make this ipv6 compatible
    //auto a = boost::asio::ip::make_address(ipAddress);
    //boost::asio::ip::tcp::endpoint e(a, std::atoi(ipAddress));

    //boost::asio::ip::tcp::socket soc(*ioc_);
    //soc.assign(boost::asio::ip::tcp::v6(), fd);

    //auto paf = kj::newPromiseAndFulfiller<void>();
    //soc.async_connect(e, [ful = std::move(paf.fulfiller)](auto ec) mutable {
    //  ful->fulfill();
    //});

    for (;;) {
      if (::connect(fd, addr, addrlen) < 0) {
        int error = errno;
        if (error == EINPROGRESS) {
          // Fine.
          break;
        } else if (error != EINTR) {
          //auto address = kj::SocketAddress(addr, addrlen).toString();
          KJ_FAIL_SYSCALL("connect()", error, ipAddress) { break; }
          return kj::Own<kj::AsyncIoStream>();
        }
      } else {
        // no error
        break;
      }
    }

    auto connected = result->on_writeable();
    // return paf.promise.then(
    return connected.then(
        kj::mvCapture(result, [fd](
                                  kj::Own<kj::AsyncIoStream> &&stream) mutable {
          int err;
          socklen_t errlen = sizeof(err);
          KJ_SYSCALL(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen));
          if (err != 0) {
            KJ_FAIL_SYSCALL("connect()", err) { break; }
          }
          //soc.release();
          return kj::mv(stream);
        }));
  }

  kj::Own<kj::ConnectionReceiver>
  wrapListenSocketFd(int fd, NetworkFilter &filter, uint flags) override {
    return kj::heap<AsioConnectionReceiver>(ioc_, fd, flags);
  }

  kj::Timer &getTimer() override {
    // TODO(soon):  Implement this.
    KJ_FAIL_ASSERT("Timers not implemented.");
  }

  auto &wait() { return wait_scope_; }

private:
  boost::asio::io_context *ioc_;
  AsioEventPort event_port_;
  kj::WaitScope wait_scope_;
};

} // namespace kja
