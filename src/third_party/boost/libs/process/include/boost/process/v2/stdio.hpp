//
// process/stdio.hpp
// ~~~~~~~~
//
// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_V2_STDIO_HPP
#define BOOST_PROCESS_V2_STDIO_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <cstddef>
#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/connect_pipe.hpp>
#else
#include <boost/asio/connect_pipe.hpp>
#endif

#if defined(BOOST_PROCESS_V2_POSIX)
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#endif

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <io.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace detail
{
#if defined(BOOST_PROCESS_V2_WINDOWS)

struct handle_closer
{
  handle_closer() = default;
  handle_closer(bool close) : close(close) {}
  handle_closer(DWORD flags) : close(false), flags{flags} {}


  void operator()(HANDLE h) const
  {
    if (close)
      ::CloseHandle(h);
    else if (flags != 0xFFFFFFFFu)
      ::SetHandleInformation(h, 0xFFFFFFFFu, flags);

  }

  bool close{false};
  DWORD flags{0xFFFFFFFFu};
};

template<DWORD Target>
struct process_io_binding
{
  HANDLE prepare()
  {
    auto hh =  h.get();
    ::SetHandleInformation(hh, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    return hh;
  }

  std::unique_ptr<void, handle_closer> h{::GetStdHandle(Target), false};

  static DWORD get_flags(HANDLE h)
  {
    DWORD res;
    if (!::GetHandleInformation(h, &res))
    {
      error_code ec;
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      throw system_error(ec, "get_flags");
    }
    return res;
  }

  process_io_binding() = default;

  template<typename Stream>
  process_io_binding(Stream && str, decltype(std::declval<Stream>().native_handle())* = nullptr)
      : process_io_binding(str.native_handle())
  {}

  process_io_binding(FILE * f) : process_io_binding(reinterpret_cast<HANDLE>(::_get_osfhandle(_fileno(f)))) {}
  process_io_binding(HANDLE h) : h{h, get_flags(h)} {}
  process_io_binding(std::nullptr_t) : process_io_binding(filesystem::path("NUL")) {}
  template<typename T, typename = typename std::enable_if<std::is_same<T, filesystem::path>::value>::type>
  process_io_binding(const T & pth)
    : h(::CreateFileW(
        pth.c_str(),
        Target == STD_INPUT_HANDLE ? GENERIC_READ : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
        ), true)
  {
  }


  template<typename Executor>
  process_io_binding(net::basic_readable_pipe<Executor> & pipe)
  {
    if (Target == STD_INPUT_HANDLE)
    {
      auto h_ = pipe.native_handle();
      h = std::unique_ptr<void, handle_closer>{h_, get_flags(h_)};
      return ;
    }

    net::detail::native_pipe_handle p[2];
    error_code ec;
    net::detail::create_pipe(p, ec);
    if (ec)
      detail::throw_error(ec, "create_pipe");
      
    h = std::unique_ptr<void, handle_closer>{p[1], true};
    pipe.assign(p[0]);
  }


  template<typename Executor>
  process_io_binding(net::basic_writable_pipe<Executor> & pipe)
  {
    if (Target != STD_INPUT_HANDLE)
    {
      auto h_ = pipe.native_handle();
      h = std::unique_ptr<void, handle_closer>{h_, get_flags(h_)};
      return ;
    }
    net::detail::native_pipe_handle p[2];
    error_code ec;
    net::detail::create_pipe(p, ec);
    if (ec)
      detail::throw_error(ec, "create_pipe");

    h = std::unique_ptr<void, handle_closer>{p[0], true};
    pipe.assign(p[1]);
  }
};

typedef process_io_binding<STD_INPUT_HANDLE>  process_input_binding;
typedef process_io_binding<STD_OUTPUT_HANDLE> process_output_binding;
typedef process_io_binding<STD_ERROR_HANDLE>  process_error_binding;

#else

template<int Target>
struct process_io_binding
{
  constexpr static int target = Target;
  int fd{target};
  bool fd_needs_closing{false};
  error_code ec;

  ~process_io_binding()
  {
    if (fd_needs_closing)
      ::close(fd);
  }

  process_io_binding() = default;
  process_io_binding(const process_io_binding &) = delete;
  process_io_binding & operator=(const process_io_binding &) = delete;

  process_io_binding(process_io_binding && other) noexcept
          : fd(other.fd), fd_needs_closing(other.fd), ec(other.ec)
  {
    other.fd = target;
    other.fd_needs_closing = false;
    other.ec = {};
  }

  process_io_binding & operator=(process_io_binding && other) noexcept
  {
    if (fd_needs_closing)
      ::close(fd);

    fd = other.fd;
    fd_needs_closing = other.fd_needs_closing;
    ec = other.ec;

    other.fd = target;
    other.fd_needs_closing = false;
    other.ec = {};
    return *this;
  }

  template<typename Stream>
  process_io_binding(Stream && str, decltype(std::declval<Stream>().native_handle()) * = nullptr)
          : process_io_binding(str.native_handle())
  {}

  process_io_binding(FILE * f) : process_io_binding(fileno(f)) {}
  process_io_binding(int fd) : fd(fd) {}
  process_io_binding(std::nullptr_t) : process_io_binding(filesystem::path("/dev/null")) {}
  process_io_binding(const filesystem::path & pth)
          : fd(::open(pth.c_str(),
                      Target == STDIN_FILENO ? O_RDONLY : (O_WRONLY | O_CREAT),
                      0660)), fd_needs_closing(true)
  {
  }

  template<typename Executor>
  process_io_binding(net::basic_readable_pipe<Executor> & readable_pipe)
  {
    if (Target == STDIN_FILENO)
    {
      fd = readable_pipe.native_handle();
      return ;
    }

    net::detail::native_pipe_handle p[2];
    net::detail::create_pipe(p, ec);
    if (ec)
      detail::throw_error(ec, "create_pipe");

    fd = p[1];
    if (::fcntl(p[0], F_SETFD, FD_CLOEXEC) == -1)
    {
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      return ;
    }
    fd_needs_closing = true;
    readable_pipe.assign(p[0], ec);
  }


  template<typename Executor>
  process_io_binding(net::basic_writable_pipe<Executor> & writable_pipe)
  {

    if (Target != STDIN_FILENO)
    {
      fd = writable_pipe.native_handle();
      return ;
    }
    net::detail::native_pipe_handle p[2];
    error_code ec;
    net::detail::create_pipe(p, ec);
    if (ec)
      detail::throw_error(ec, "create_pipe");

    fd = p[0];
    if (::fcntl(p[1], F_SETFD, FD_CLOEXEC) == -1)
    {
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      return ;
    }
    fd_needs_closing = true;
    writable_pipe.assign(p[1], ec);
  }

  error_code on_setup(posix::default_launcher &,
                      const filesystem::path &, const char * const *)
  {
      return ec;
  }

  error_code on_exec_setup(posix::default_launcher & launcher,
                           const filesystem::path &, const char * const *)
  {
    if (::dup2(fd, target) == -1)
      return get_last_error();
    else
      return error_code();
  }
};

typedef process_io_binding<STDIN_FILENO>  process_input_binding;
typedef process_io_binding<STDOUT_FILENO> process_output_binding;
typedef process_io_binding<STDERR_FILENO> process_error_binding;

#endif

}


/// The initializer for the stdio of a subprocess
/** The subprocess initializer has three members:
 * 
 *  - in for stdin
 *  - out for stdout
 *  - err for stderr
 * 
 * If the initializer is present all three will be set for the subprocess.
 * By default they will inherit the stdio handles from the parent process. 
 * This means that this will forward stdio to the subprocess:
 * 
 * @code {.cpp}
 * asio::io_context ctx;
 * v2::process proc(ctx, "/bin/bash", {}, v2::process_stdio{});
 * @endcode
 * 
 * No constructors are provided in order to support designated initializers
 * in later version of C++.
 * 
 * * @code {.cpp}
 * asio::io_context ctx;
 * /// C++17
 * v2::process proc17(ctx, "/bin/bash", {}, v2::process_stdio{.stderr=nullptr});
 * /// C++11 & C++14
 * v2::process proc17(ctx, "/bin/bash", {}, v2::process_stdio{ {}, {}, nullptr});
 *                                                        stdin ^  ^ stderr
 * @endcode
 * 
 * Valid initializers for any stdio are:
 * 
 *  - `std::nullptr_t` assigning a null-device
 *  - `FILE*` any open file, including `stdin`, `stdout` and `stderr`
 *  - a filesystem::path, which will open a readable or writable depending on the direction of the stream
 *  - `native_handle` any native file handle (`HANDLE` on windows) or file descriptor (`int` on posix)
 *  - any io-object with a .native_handle() function that is compatible with the above. E.g. a asio::ip::tcp::socket
 *  - an asio::basic_writeable_pipe for stdin or asio::basic_readable_pipe for stderr/stdout. 
 * 
 * 
 */ 
struct process_stdio
{
  detail::process_input_binding in;
  detail::process_output_binding out;
  detail::process_error_binding err;

#if defined(BOOST_PROCESS_V2_WINDOWS)
  error_code on_setup(windows::default_launcher & launcher, const filesystem::path &, const std::wstring &)
  {
    launcher.startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    launcher.startup_info.StartupInfo.hStdInput  = in.prepare();
    launcher.startup_info.StartupInfo.hStdOutput = out.prepare();
    launcher.startup_info.StartupInfo.hStdError  = err.prepare();
    launcher.inherited_handles.reserve(launcher.inherited_handles.size() + 3);
    launcher.inherited_handles.push_back(launcher.startup_info.StartupInfo.hStdInput);
    launcher.inherited_handles.push_back(launcher.startup_info.StartupInfo.hStdOutput);
    launcher.inherited_handles.push_back(launcher.startup_info.StartupInfo.hStdError);
    return error_code {};
  };
#else
  error_code on_exec_setup(posix::default_launcher & /*launcher*/, const filesystem::path &, const char * const *)
  {
    if (::dup2(in.fd, in.target) == -1)
      return error_code(errno, system_category());

    if (::dup2(out.fd, out.target) == -1)
      return error_code(errno, system_category());

    if (::dup2(err.fd, err.target) == -1)
      return error_code(errno, system_category());

    return error_code {};
  };
#endif

};

BOOST_PROCESS_V2_END_NAMESPACE

#endif //  BOOST_PROCESS_V2_STDIO_HPP