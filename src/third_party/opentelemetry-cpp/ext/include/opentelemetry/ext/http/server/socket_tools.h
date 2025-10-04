// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

// #  include <Windows.h>

#  include <winsock2.h>

// TODO: consider NOMINMAX
#  undef min
#  undef max
#  pragma comment(lib, "ws2_32.lib")

#else

#  include <unistd.h>

#  ifdef __linux__
#    include <sys/epoll.h>
#  endif

#  ifdef __APPLE__
#    include "TargetConditionals.h"
// Use kqueue on mac
#    include <sys/event.h>
#    include <sys/time.h>
#    include <sys/types.h>
#  endif

// Common POSIX headers for Linux and Mac OS X
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>

#endif

#ifndef _Out_cap_
#  define _Out_cap_(size)
#endif

#if defined(HAVE_CONSOLE_LOG) && !defined(LOG_DEBUG)
// Log to console if there's no standard log facility defined
#  include <cstdio>
#  ifndef LOG_DEBUG
#    define LOG_DEBUG(fmt_, ...) printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_TRACE(fmt_, ...) printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_INFO(fmt_, ...) printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_WARN(fmt_, ...) printf(" " fmt_ "\n", ##__VA_ARGS__)
#    define LOG_ERROR(fmt_, ...) printf(" " fmt_ "\n", ##__VA_ARGS__)
#  endif
#endif

#ifndef LOG_DEBUG
// Don't log anything if there's no standard log facility defined
#  define LOG_DEBUG(fmt_, ...)
#  define LOG_TRACE(fmt_, ...)
#  define LOG_INFO(fmt_, ...)
#  define LOG_WARN(fmt_, ...)
#  define LOG_ERROR(fmt_, ...)
#endif

namespace common
{

/// <summary>
/// A simple thread, derived class overloads onThread() method.
/// </summary>
struct Thread
{
  std::thread m_thread;

  std::atomic<bool> m_terminate{false};

  /// <summary>
  /// Thread Constructor
  /// </summary>
  /// <returns>Thread</returns>
  Thread() {}

  /// <summary>
  /// Start Thread
  /// </summary>
  void startThread()
  {
    m_terminate = false;
    m_thread    = std::thread([&]() { this->onThread(); });
  }

  /// <summary>
  /// Join Thread
  /// </summary>
  void joinThread()
  {
    m_terminate = true;
    if (m_thread.joinable())
    {
      m_thread.join();
    }
  }

  /// <summary>
  /// Indicates if this thread should terminate
  /// </summary>
  /// <returns></returns>
  bool shouldTerminate() const { return m_terminate; }

  /// <summary>
  /// Must be implemented by children
  /// </summary>
  virtual void onThread() = 0;

  /// <summary>
  /// Thread destructor
  /// </summary>
  /// <returns></returns>
  virtual ~Thread() noexcept {}
};

}  // namespace common
namespace SocketTools
{

#ifdef _WIN32
// WinSocks need extra (de)initialization, solved by a global object here,
// whose constructor/destructor will be called before and after main().
struct WsaInitializer
{
  WsaInitializer()
  {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
  }

  ~WsaInitializer() { WSACleanup(); }
};

static WsaInitializer g_wsaInitializer;

#endif

/// <summary>
/// Encapsulation of sockaddr(_in)
/// </summary>
struct SocketAddr
{
  static u_long const Loopback = 0x7F000001;

  sockaddr m_data;

  /// <summary>
  /// SocketAddr constructor
  /// </summary>
  /// <returns>SocketAddr</returns>
  SocketAddr() { memset(&m_data, 0, sizeof(m_data)); }

  SocketAddr(u_long addr, int port)
  {
    sockaddr_in &inet4    = reinterpret_cast<sockaddr_in &>(m_data);
    inet4.sin_family      = AF_INET;
    inet4.sin_port        = htons(static_cast<unsigned short>(port));
    inet4.sin_addr.s_addr = htonl(addr);
  }

  SocketAddr(char const *addr)
  {
#ifdef _WIN32
    INT addrlen = sizeof(m_data);
    WCHAR buf[200];
    for (int i = 0; i < sizeof(buf) && addr[i]; i++)
    {
      buf[i] = addr[i];
    }
    buf[199] = L'\0';
    ::WSAStringToAddressW(buf, AF_INET, nullptr, &m_data, &addrlen);
#else
    sockaddr_in &inet4 = reinterpret_cast<sockaddr_in &>(m_data);
    inet4.sin_family   = AF_INET;
    char const *colon  = strchr(addr, ':');
    if (colon)
    {
      inet4.sin_port = htons(atoi(colon + 1));
      char buf[16];
      memcpy(buf, addr, (std::min<ptrdiff_t>)(15, colon - addr));
      buf[15] = '\0';
      ::inet_pton(AF_INET, buf, &inet4.sin_addr);
    }
    else
    {
      inet4.sin_port = 0;
      ::inet_pton(AF_INET, addr, &inet4.sin_addr);
    }
#endif
  }

  SocketAddr(SocketAddr const &other) = default;

  SocketAddr &operator=(SocketAddr const &other) = default;

  operator sockaddr *() { return &m_data; }

  operator const sockaddr *() const { return &m_data; }

  int port() const
  {
    switch (m_data.sa_family)
    {
      case AF_INET: {
        sockaddr_in const &inet4 = reinterpret_cast<sockaddr_in const &>(m_data);
        return ntohs(inet4.sin_port);
      }

      default:
        return -1;
    }
  }

  std::string toString() const
  {
    std::ostringstream os;

    switch (m_data.sa_family)
    {
      case AF_INET: {
        sockaddr_in const &inet4 = reinterpret_cast<sockaddr_in const &>(m_data);
        u_long addr              = ntohl(inet4.sin_addr.s_addr);
        os << (addr >> 24) << '.' << ((addr >> 16) & 255) << '.' << ((addr >> 8) & 255) << '.'
           << (addr & 255);
        os << ':' << ntohs(inet4.sin_port);
        break;
      }

      default:
        os << "[?AF?" << m_data.sa_family << ']';
    }
    return os.str();
  }
};

/// <summary>
/// Encapsulation of a socket (non-exclusive ownership)
/// </summary>
struct Socket
{
#ifdef _WIN32
  typedef SOCKET Type;
  static Type const Invalid = INVALID_SOCKET;
#else
  typedef int Type;
  static Type const Invalid = -1;
#endif

  Type m_sock;

  Socket(Type sock = Invalid) : m_sock(sock) {}

  Socket(int af, int type, int proto) { m_sock = ::socket(af, type, proto); }

  ~Socket() {}

  operator Socket::Type() const { return m_sock; }

  bool operator==(Socket const &other) const { return (m_sock == other.m_sock); }

  bool operator!=(Socket const &other) const { return (m_sock != other.m_sock); }

  bool operator<(Socket const &other) const { return (m_sock < other.m_sock); }

  bool invalid() const { return (m_sock == Invalid); }

  void setNonBlocking()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    u_long value = 1;
    ::ioctlsocket(m_sock, FIONBIO, &value);
#else
    int flags = ::fcntl(m_sock, F_GETFL, 0);
    ::fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
#endif
  }

  bool setReuseAddr()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    BOOL value = TRUE;
#else
    int value = 1;
#endif
    return (::setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&value),
                         sizeof(value)) == 0);
  }

  bool setNoDelay()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    BOOL value = TRUE;
#else
    int value = 1;
#endif
    return (::setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&value),
                         sizeof(value)) == 0);
  }

  bool connect(SocketAddr const &addr)
  {
    assert(m_sock != Invalid);
    return (::connect(m_sock, addr, sizeof(addr)) == 0);
  }

  void close()
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    ::closesocket(m_sock);
#else
    ::close(m_sock);
#endif
    m_sock = Invalid;
  }

  int recv(_Out_cap_(size) void *buffer, unsigned size)
  {
    assert(m_sock != Invalid);
    int flags = 0;
    return static_cast<int>(::recv(m_sock, reinterpret_cast<char *>(buffer), size, flags));
  }

  int send(void const *buffer, unsigned size)
  {
    assert(m_sock != Invalid);
    return static_cast<int>(::send(m_sock, reinterpret_cast<char const *>(buffer), size, 0));
  }

  bool bind(SocketAddr const &addr)
  {
    assert(m_sock != Invalid);
    return (::bind(m_sock, addr, sizeof(addr)) == 0);
  }

  bool getsockname(SocketAddr &addr) const
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif
    return (::getsockname(m_sock, addr, &addrlen) == 0);
  }

  bool listen(int backlog)
  {
    assert(m_sock != Invalid);
    return (::listen(m_sock, backlog) == 0);
  }

  bool accept(Socket &csock, SocketAddr &caddr)
  {
    assert(m_sock != Invalid);
#ifdef _WIN32
    int addrlen = sizeof(caddr);
#else
    socklen_t addrlen = sizeof(caddr);
#endif
    csock = ::accept(m_sock, caddr, &addrlen);
    return !csock.invalid();
  }

  bool shutdown(int how)
  {
    assert(m_sock != Invalid);
    return (::shutdown(m_sock, how) == 0);
  }

  int error() const
  {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
  }

  enum
  {
#ifdef _WIN32
    ErrorWouldBlock = WSAEWOULDBLOCK
#else
    ErrorWouldBlock = EWOULDBLOCK
#endif
  };

  enum
  {
#ifdef _WIN32
    ShutdownReceive = SD_RECEIVE,
    ShutdownSend    = SD_SEND,
    ShutdownBoth    = SD_BOTH
#else
    ShutdownReceive = SHUT_RD,
    ShutdownSend    = SHUT_WR,
    ShutdownBoth    = SHUT_RDWR
#endif
  };
};

/// <summary>
/// Socket Data
/// </summary>
struct SocketData
{
  Socket socket;
  int flags;

  SocketData() : socket(), flags(0) {}

  bool operator==(Socket s) { return (socket == s); }
};

/// <summary>
/// Socket Reactor
/// </summary>
struct Reactor : protected common::Thread
{
  /// <summary>
  /// Socket State callback
  /// </summary>
  class SocketCallback
  {
  public:
    SocketCallback()                             = default;
    virtual ~SocketCallback()                    = default;
    virtual void onSocketReadable(Socket sock)   = 0;
    virtual void onSocketWritable(Socket sock)   = 0;
    virtual void onSocketAcceptable(Socket sock) = 0;
    virtual void onSocketClosed(Socket sock)     = 0;
  };

  /// <summary>
  /// Socket State
  /// </summary>
  enum State
  {
    Readable   = 1,
    Writable   = 2,
    Acceptable = 4,
    Closed     = 8
  };

  SocketCallback &m_callback;

  std::vector<SocketData> m_sockets;

#ifdef _WIN32
  /* use WinSock events on Windows */
  std::vector<WSAEVENT> m_events{};
#endif

#ifdef __linux__
  /* use epoll on Linux */
  int m_epollFd;
#endif

#ifdef TARGET_OS_MAC
  /* use kqueue on Mac */
#  define KQUEUE_SIZE 32
  int kq{0};
  struct kevent m_events[KQUEUE_SIZE];
#endif

public:
  Reactor(SocketCallback &callback) : m_callback(callback)
  {
#ifdef __linux__
#  ifdef ANDROID
    m_epollFd = ::epoll_create(0);
#  else
    m_epollFd = ::epoll_create1(0);
#  endif
#endif

#ifdef TARGET_OS_MAC
    bzero(&m_events[0], sizeof(m_events));
    kq = kqueue();
#endif
  }

  ~Reactor() override
  {
#ifdef __linux__
    ::close(m_epollFd);
#endif
#ifdef TARGET_OS_MAC
    ::close(kq);
#endif
  }

  /// <summary>
  /// Add Socket
  /// </summary>
  /// <param name="socket"></param>
  /// <param name="flags"></param>
  void addSocket(const Socket &socket, int flags)
  {
    if (flags == 0)
    {
      removeSocket(socket);
    }
    else
    {
      auto it = std::find(m_sockets.begin(), m_sockets.end(), socket);
      if (it == m_sockets.end())
      {
        LOG_TRACE("Reactor: Adding socket 0x%x with flags 0x%x", static_cast<int>(socket), flags);
#ifdef _WIN32
        m_events.push_back(::WSACreateEvent());
#endif
#ifdef __linux__
        epoll_event event = {};
        event.data.fd     = socket;
        event.events      = 0;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, socket, &event);
#endif
#ifdef TARGET_OS_MAC
        struct kevent event;
        bzero(&event, sizeof(event));
        event.ident = socket.m_sock;
        EV_SET(&event, event.ident, EVFILT_READ, EV_ADD, 0, 0, NULL);
        kevent(kq, &event, 1, NULL, 0, NULL);
        EV_SET(&event, event.ident, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        kevent(kq, &event, 1, NULL, 0, NULL);
#endif
        m_sockets.push_back(SocketData());
        m_sockets.back().socket = socket;
        m_sockets.back().flags  = 0;
        it                      = m_sockets.end() - 1;
      }
      else
      {
        LOG_TRACE("Reactor: Updating socket 0x%x with flags 0x%x", static_cast<int>(socket), flags);
      }

      if (it->flags != flags)
      {
        it->flags = flags;
#ifdef _WIN32
        long lNetworkEvents = 0;
        if (it->flags & Readable)
        {
          lNetworkEvents |= FD_READ;
        }
        if (it->flags & Writable)
        {
          lNetworkEvents |= FD_WRITE;
        }
        if (it->flags & Acceptable)
        {
          lNetworkEvents |= FD_ACCEPT;
        }
        if (it->flags & Closed)
        {
          lNetworkEvents |= FD_CLOSE;
        }
        auto eventIt = m_events.begin() + std::distance(m_sockets.begin(), it);
        ::WSAEventSelect(socket, *eventIt, lNetworkEvents);
#endif
#ifdef __linux__
        int events = 0;
        if (it->flags & Readable)
        {
          events |= EPOLLIN;
        }
        if (it->flags & Writable)
        {
          events |= EPOLLOUT;
        }
        if (it->flags & Acceptable)
        {
          events |= EPOLLIN;
        }
        // if (it->flags & Closed) - always handled (EPOLLERR | EPOLLHUP)
        epoll_event event = {};
        event.data.fd     = socket;
        event.events      = events;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, socket, &event);
#endif
#ifdef TARGET_OS_MAC
        // TODO: [MG] - Mac OS X socket doesn't currently support updating flags
#endif
      }
    }
  }

  /// <summary>
  /// Remove Socket
  /// </summary>
  /// <param name="socket"></param>
  void removeSocket(const Socket &socket)
  {
    LOG_TRACE("Reactor: Removing socket 0x%x", static_cast<int>(socket));
    auto it = std::find(m_sockets.begin(), m_sockets.end(), socket);
    if (it != m_sockets.end())
    {
#ifdef _WIN32
      auto eventIt = m_events.begin() + std::distance(m_sockets.begin(), it);
      ::WSAEventSelect(it->socket, *eventIt, 0);
      ::WSACloseEvent(*eventIt);
      m_events.erase(eventIt);
#endif
#ifdef __linux__
      ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, socket, nullptr);
#endif
#ifdef TARGET_OS_MAC
      struct kevent event;
      bzero(&event, sizeof(event));
      event.ident = socket;
      EV_SET(&event, socket, EVFILT_READ, EV_DELETE, 0, 0, NULL);
      if (-1 == kevent(kq, &event, 1, NULL, 0, NULL))
      {
        //// Already removed?
        LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
      }
      EV_SET(&event, socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
      if (-1 == kevent(kq, &event, 1, NULL, 0, NULL))
      {
        //// Already removed?
        LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
      }
#endif
      m_sockets.erase(it);
    }
  }

  /// <summary>
  /// Start server
  /// </summary>
  void start()
  {
    LOG_INFO("Reactor: Starting...");
    startThread();
  }

  /// <summary>
  /// Stop server
  /// </summary>
  void stop()
  {
    LOG_INFO("Reactor: Stopping...");
    joinThread();
#ifdef _WIN32
    for (auto &hEvent : m_events)
    {
      ::WSACloseEvent(hEvent);
    }
#else /* Linux and Mac */
    for (auto &sd : m_sockets)
    {
#  ifdef __linux__
      ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, sd.socket, nullptr);
#  endif
#  ifdef TARGET_OS_MAC
      struct kevent event;
      bzero(&event, sizeof(event));
      event.ident = sd.socket;
      EV_SET(&event, sd.socket, EVFILT_READ, EV_DELETE, 0, 0, NULL);
      if (-1 == kevent(kq, &event, 1, NULL, 0, NULL))
      {
        LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
      }
      EV_SET(&event, sd.socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
      if (-1 == kevent(kq, &event, 1, NULL, 0, NULL))
      {
        LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
      }
#  endif
    }
#endif
    m_sockets.clear();
  }

  /// <summary>
  /// Thread Loop for async events processing
  /// </summary>
  virtual void onThread() override
  {
    LOG_INFO("Reactor: Thread started");
    while (!shouldTerminate())
    {
#ifdef _WIN32
      DWORD dwResult = ::WSAWaitForMultipleEvents(static_cast<DWORD>(m_events.size()),
                                                  m_events.data(), FALSE, 500, FALSE);
      if (dwResult == WSA_WAIT_TIMEOUT)
      {
        continue;
      }

      assert(dwResult <= WSA_WAIT_EVENT_0 + m_events.size());
      int index     = dwResult - WSA_WAIT_EVENT_0;
      Socket socket = m_sockets[index].socket;
      int flags     = m_sockets[index].flags;

      WSANETWORKEVENTS ne;
      ::WSAEnumNetworkEvents(socket, m_events[index], &ne);
      LOG_TRACE(
          "Reactor: Handling socket 0x%x (index %d) with active flags 0x%x "
          "(armed 0x%x)",
          static_cast<int>(socket), index, ne.lNetworkEvents, flags);

      if ((flags & Readable) && (ne.lNetworkEvents & FD_READ))
      {
        m_callback.onSocketReadable(socket);
      }
      if ((flags & Writable) && (ne.lNetworkEvents & FD_WRITE))
      {
        m_callback.onSocketWritable(socket);
      }
      if ((flags & Acceptable) && (ne.lNetworkEvents & FD_ACCEPT))
      {
        m_callback.onSocketAcceptable(socket);
      }
      if ((flags & Closed) && (ne.lNetworkEvents & FD_CLOSE))
      {
        m_callback.onSocketClosed(socket);
      }
#endif

#ifdef __linux__
      epoll_event events[4];
      int result = ::epoll_wait(m_epollFd, events, sizeof(events) / sizeof(events[0]), 500);
      if (result == 0 || (result == -1 && errno == EINTR))
      {
        continue;
      }

      assert(result >= 1 && static_cast<size_t>(result) <= sizeof(events) / sizeof(events[0]));
      for (int i = 0; i < result; i++)
      {
        auto it = std::find(m_sockets.begin(), m_sockets.end(), events[i].data.fd);
        assert(it != m_sockets.end());
        Socket socket = it->socket;
        int flags     = it->flags;

        LOG_TRACE("Reactor: Handling socket 0x%x active flags 0x%x (armed 0x%x)",
                  static_cast<int>(socket), events[i].events, flags);

        if ((flags & Readable) && (events[i].events & EPOLLIN))
        {
          m_callback.onSocketReadable(socket);
        }
        if ((flags & Writable) && (events[i].events & EPOLLOUT))
        {
          m_callback.onSocketWritable(socket);
        }
        if ((flags & Acceptable) && (events[i].events & EPOLLIN))
        {
          m_callback.onSocketAcceptable(socket);
        }
        if ((flags & Closed) && (events[i].events & (EPOLLHUP | EPOLLERR)))
        {
          m_callback.onSocketClosed(socket);
        }
      }
#endif

#if defined(TARGET_OS_MAC)
      unsigned waitms = 500;  // never block for more than 500ms
      struct timespec timeout;
      timeout.tv_sec  = waitms / 1000;
      timeout.tv_nsec = (waitms % 1000) * 1000 * 1000;

      int nev = kevent(kq, NULL, 0, m_events, KQUEUE_SIZE, &timeout);
      for (int i = 0; i < nev; i++)
      {
        struct kevent &event = m_events[i];
        int fd               = (int)event.ident;
        auto it              = std::find(m_sockets.begin(), m_sockets.end(), fd);
        assert(it != m_sockets.end());
        Socket socket = it->socket;
        int flags     = it->flags;

        LOG_TRACE("Handling socket 0x%x active flags 0x%x (armed 0x%x)", static_cast<int>(socket),
                  event.flags, event.fflags);

        if (event.filter == EVFILT_READ)
        {
          if (flags & Acceptable)
          {
            m_callback.onSocketAcceptable(socket);
          }
          if (flags & Readable)
          {
            m_callback.onSocketReadable(socket);
          }
          continue;
        }

        if (event.filter == EVFILT_WRITE)
        {
          if (flags & Writable)
          {
            m_callback.onSocketWritable(socket);
          }
          continue;
        }

        if ((event.flags & EV_EOF) || (event.flags & EV_ERROR))
        {
          LOG_TRACE("event.filter=%s", "EVFILT_WRITE");
          m_callback.onSocketClosed(socket);
          it->flags = Closed;
          struct kevent kevt;
          EV_SET(&kevt, event.ident, EVFILT_READ, EV_DELETE, 0, 0, NULL);
          if (-1 == kevent(kq, &kevt, 1, NULL, 0, NULL))
          {
            LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
          }
          EV_SET(&kevt, event.ident, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
          if (-1 == kevent(kq, &kevt, 1, NULL, 0, NULL))
          {
            LOG_ERROR("cannot delete fd=0x%x from kqueue!", event.ident);
          }
          continue;
        }
        LOG_ERROR("Reactor: unhandled kevent!");
      }
#endif
    }
    LOG_TRACE("Reactor: Thread done");
  }
};

}  // namespace SocketTools
