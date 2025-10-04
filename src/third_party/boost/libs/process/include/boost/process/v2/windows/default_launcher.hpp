//
// boost/process/v2/windows/default_launcher.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_WINDOWS_DEFAULT_LAUNCHER_HPP
#define BOOST_PROCESS_V2_WINDOWS_DEFAULT_LAUNCHER_HPP

#include <boost/process/v2/cstring_ref.hpp>
#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/detail/utf8.hpp>
#include <boost/process/v2/error.hpp>

#include <numeric>
#include <memory>
#include <type_traits>
#include <windows.h>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/execution/executor.hpp>
#include <asio/is_executor.hpp>
#include <asio/execution_context.hpp>
#else
#include <boost/asio/execution/executor.hpp>
#include <boost/asio/is_executor.hpp>
#include <boost/asio/execution_context.hpp>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

template<typename Executor>
struct basic_process;

namespace detail
{

struct base {};
struct derived : base {};

template<typename Launcher, typename Init>
inline error_code invoke_on_setup(Launcher & /*launcher*/, const filesystem::path &executable, std::wstring &cmd_line,
                                  Init && /*init*/, base && )
{
  return error_code{};
}

template<typename Launcher, typename Init>
inline auto invoke_on_setup(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                            Init && init, derived && )
-> decltype(init.on_setup(launcher, executable, cmd_line))
{
  return init.on_setup(launcher, executable, cmd_line);
}

template<typename Launcher, typename Init>
inline std::false_type probe_on_setup(
    Launcher & launcher, Init && init, base && );

template<typename Launcher, typename Init>
inline auto probe_on_setup(Launcher & launcher, Init && init, derived && )
        -> std::is_same<error_code, decltype(init.on_setup(launcher, std::declval<const filesystem::path &>(), std::declval<std::wstring &>()))>;

template<typename Launcher, typename Init>
using has_on_setup = decltype(probe_on_setup(std::declval<Launcher&>(), std::declval<Init>(), derived{}));

template<typename Launcher>
inline error_code on_setup(Launcher & /*launcher*/, const filesystem::path &/*executable*/, std::wstring &/*cmd_line*/)
{
  return error_code{};
}

template<typename Launcher, typename Init1, typename ... Inits>
inline error_code on_setup(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                           Init1 && init1, Inits && ... inits)
{
  auto ec = invoke_on_setup(launcher, executable, cmd_line, init1, derived{});
  if (ec)
    return ec;
  else
    return on_setup(launcher, executable, cmd_line, inits...);
}


template<typename Launcher, typename Init>
inline void invoke_on_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/, std::wstring &/*cmd_line*/,
                            const error_code & /*ec*/, Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_error(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                            const error_code & ec, Init && init, derived && )
-> decltype(init.on_error(launcher, executable, cmd_line, ec))
{
  init.on_error(launcher, executable, cmd_line, ec);
}


template<typename Launcher, typename Init>
inline std::false_type probe_on_error(
    Launcher & launcher, Init && init, base && );

template<typename Launcher, typename Init>
inline auto probe_on_error(Launcher & launcher, Init && init, derived && )
        -> std::is_same<error_code, decltype(init.on_error(launcher, std::declval<const filesystem::path &>(), std::declval<std::wstring &>(), std::declval<std::error_code&>()))>;

template<typename Launcher, typename Init>
using has_on_error = decltype(probe_on_error(std::declval<Launcher&>(), std::declval<Init>(), derived{}));


template<typename Launcher>
inline void on_error(Launcher & /*launcher*/, const filesystem::path &/*executable*/, std::wstring &/*cmd_line*/,
                     const error_code & /*ec*/)
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_error(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                     const error_code & ec,
                     Init1 && init1, 
                     Inits && ... inits)
{
  invoke_on_error(launcher, executable, cmd_line, ec, init1, derived{});
  on_error(launcher, executable, cmd_line, ec, inits...);
}

template<typename Launcher, typename Init>
inline void invoke_on_success(Launcher & /*launcher*/, const filesystem::path &/*executable*/, std::wstring &/*cmd_line*/,
                              Init && /*init*/, base && )
{
}

template<typename Launcher, typename Init>
inline auto invoke_on_success(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                              Init && init, derived && )
    -> decltype(init.on_success(launcher, executable, cmd_line))
{
  init.on_success(launcher, executable, cmd_line);
}

template<typename Launcher, typename Init>
inline std::false_type probe_on_success(
    Launcher & launcher, Init && init, base && );

template<typename Launcher, typename Init>
inline auto probe_on_success(Launcher & launcher, Init && init, derived && )
        -> std::is_same<error_code, decltype(init.on_success(launcher, std::declval<const filesystem::path &>(), std::declval<std::wstring &>()))>;

template<typename Launcher, typename Init>
using has_on_success = decltype(probe_on_success(std::declval<Launcher&>(), std::declval<Init>(), derived{}));

template<typename Launcher>
inline void on_success(Launcher & /*launcher*/, const filesystem::path &/*executable*/, std::wstring &/*cmd_line*/)
{
}

template<typename Launcher, typename Init1, typename ... Inits>
inline void on_success(Launcher & launcher, const filesystem::path &executable, std::wstring &cmd_line,
                       Init1 && init1, Inits && ... inits)
{
  invoke_on_success(launcher, executable, cmd_line, init1, derived{});
  on_success(launcher, executable, cmd_line, inits...);
}

template<typename Launcher, typename Init>
struct is_initializer : std::integral_constant<bool, 
    has_on_setup<Launcher, Init>::value || 
    has_on_error<Launcher, Init>::value || 
    has_on_success<Launcher, Init>::value>
{
};

template<typename Launcher, typename ... Inits>
struct all_are_initializers;

template<typename Launcher>
struct all_are_initializers<Launcher> : std::true_type {};


template<typename Launcher, typename Init>
struct all_are_initializers<Launcher, Init> : is_initializer<Launcher, Init> {};

template<typename Launcher, typename Init, typename ... Tail>
struct all_are_initializers<Launcher, Init, Tail...> 
  : std::integral_constant<bool,  is_initializer<Launcher, Init>::value && all_are_initializers<Launcher, Tail...>::value>
{
};


}

template<typename Executor>
struct basic_process;

namespace windows
{

/// The default launcher for processes on windows.
struct default_launcher
{
  //// The process_attributes passed to CreateProcess
  SECURITY_ATTRIBUTES * process_attributes = nullptr;
  //// The thread_attributes passed to CreateProcess
  SECURITY_ATTRIBUTES * thread_attributes = nullptr;
  /// The inhreited_handles option. bInheritHandles will be true if not empty..
  std::vector<HANDLE> inherited_handles;
  /// The creation flags of the process. Initializers may add to them; extended startupinfo is assumed.
  DWORD creation_flags{EXTENDED_STARTUPINFO_PRESENT};
  /// A pointer to the subprocess environment.
  void * environment = nullptr;
  /// The startup director. An empty path will get ignored.
  filesystem::path current_directory{};

  /// The full startup info passed to CreateProcess
  STARTUPINFOEXW startup_info{{sizeof(STARTUPINFOEXW), nullptr, nullptr, nullptr,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, nullptr,
                              INVALID_HANDLE_VALUE,
                              INVALID_HANDLE_VALUE,
                              INVALID_HANDLE_VALUE},
                              nullptr};
  /// The process_information that gets assigned after a call to CreateProcess
  PROCESS_INFORMATION process_information{nullptr, nullptr, 0,0};

  template<typename Executor, typename ... Inits>
  using enable_init = typename std::enable_if< 
                                    detail::all_are_initializers<default_launcher, Inits...>::value, 
                                    basic_process<Executor>>::type;

  default_launcher() = default;

  template<typename ExecutionContext, typename Args, typename ... Inits>
  auto operator()(ExecutionContext & context,
                  const typename std::enable_if<std::is_convertible<
                             ExecutionContext&, net::execution_context&>::value,
                             filesystem::path >::type & executable,
                  Args && args,
                  Inits && ... inits ) -> enable_init<typename ExecutionContext::executor_type, Inits...>
  {
      error_code ec;
      auto proc =  (*this)(context, ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);

      if (ec)
          v2::detail::throw_error(ec, "default_launcher");

      return proc;
  }


  template<typename ExecutionContext, typename Args, typename ... Inits>
  auto operator()(ExecutionContext & context,
                  error_code & ec,
                  const typename std::enable_if<std::is_convertible<
                             ExecutionContext&, net::execution_context&>::value,
                             filesystem::path >::type & executable,
                  Args && args,
                  Inits && ... inits ) -> enable_init<typename ExecutionContext::executor_type, Inits...>
  {
      return (*this)(context.get_executor(), ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);
  }

  template<typename Executor, typename Args, typename ... Inits>
  auto operator()(Executor exec,
                  const typename std::enable_if<
                             net::execution::is_executor<Executor>::value
                          || net::is_executor<Executor>::value,
                             filesystem::path >::type & executable,
                  Args && args,
                  Inits && ... inits ) -> enable_init<Executor, Inits...>
  {
      error_code ec;
      auto proc =  (*this)(std::move(exec), ec, executable, std::forward<Args>(args), std::forward<Inits>(inits)...);

      if (ec)
          detail::throw_error(ec, "default_launcher");

      return proc;
  }

  template<typename Executor, typename Args, typename ... Inits>
  auto operator()(Executor exec,
                  error_code & ec,
                  const typename std::enable_if<
                             net::execution::is_executor<Executor>::value ||
                             net::is_executor<Executor>::value,
                             filesystem::path >::type & executable,
                  Args && args,
                  Inits && ... inits ) -> enable_init<Executor, Inits...>
  {
    auto command_line = this->build_command_line(executable, std::forward<Args>(args));

    ec = detail::on_setup(*this, executable, command_line, inits...);

    if (ec)
    {
      detail::on_error(*this, executable, command_line, ec, inits...);
      return basic_process<Executor>(exec);
    }

    if (!inherited_handles.empty())
    {
      set_handle_list(ec);
      if (ec)
        return basic_process<Executor>(exec);
    }

    auto ok = ::CreateProcessW(
        executable.empty() ? nullptr : executable.c_str(),
        command_line.empty() ? nullptr : &command_line.front(),
        process_attributes,
        thread_attributes,
        inherited_handles.empty() ? FALSE : TRUE,
        creation_flags,
        environment,
        current_directory.empty() ? nullptr : current_directory.c_str(),
        &startup_info.StartupInfo,
        &process_information);

    if (ok == 0)
    {
      BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
      detail::on_error(*this, executable, command_line, ec, inits...);

      if (process_information.hProcess != INVALID_HANDLE_VALUE)
        ::CloseHandle(process_information.hProcess);
      if (process_information.hThread != INVALID_HANDLE_VALUE)
        ::CloseHandle(process_information.hThread);

      return basic_process<Executor>(exec);
    }
    else
    {
      detail::on_success(*this, executable, command_line, inits...);

      if (process_information.hThread != INVALID_HANDLE_VALUE)
        ::CloseHandle(process_information.hThread);

      return basic_process<Executor>(exec,
                     this->process_information.dwProcessId,
                     this->process_information.hProcess);
    }
  }
  
  BOOST_PROCESS_V2_DECL static 
  std::size_t escaped_argv_length(basic_string_view<wchar_t> ws);
  BOOST_PROCESS_V2_DECL static 
  std::size_t escape_argv_string(wchar_t * itr, std::size_t max_size, 
                                 basic_string_view<wchar_t> ws);
                                        



  template<typename Argv>
  static std::wstring build_command_line_impl(
              const filesystem::path & pt, 
              const Argv & argv, 
              basic_string_view<wchar_t> args)
  {
      std::size_t req_size = std::accumulate(
          std::begin(argv), std::end(argv),  escaped_argv_length(pt.native()),
          [](std::size_t sz, basic_string_view<wchar_t> arg) -> std::size_t
          {
              return sz + 1u + escaped_argv_length(arg);
          });

      std::wstring res;
      res.resize(req_size, L' ');
      
      wchar_t * itr = &res.front();
      itr += escape_argv_string(itr, res.size(), pt.native());
      for (const auto & a : argv)
      {
        itr++;
        itr += escape_argv_string(itr, std::distance(itr, &res.back() + 1), a);
      }
      return res;
  }

  template<typename Argv>
  static std::wstring build_command_line_impl(
              const filesystem::path & pt, 
              const Argv & argv,
              basic_string_view<char> args)
  {
    std::vector<std::wstring> argw;
    argw.resize(std::distance(std::begin(argv), std::end(argv)));
    std::transform(std::begin(argv), std::end(argv), argw.begin(), 
                   [](basic_string_view <char> arg)
                   {
                      return detail::conv_string<wchar_t>(arg.data(), arg.size());
                   });
    return build_command_line_impl(pt, argw, L"");
  }

  template<typename Args,
           typename Char = decltype(*std::begin(std::declval<Args>()))>
  static std::wstring build_command_line(const filesystem::path & pt, const Args & args)
  {
    if (std::begin(args) == std::end(args))
      return pt.native();

    return build_command_line_impl(pt, args, *std::begin(args));
  }

  static std::wstring build_command_line(const filesystem::path & pt, const wchar_t * args)
  {
    return args;
  }

  struct lpproc_thread_closer
  {
    void operator()(::LPPROC_THREAD_ATTRIBUTE_LIST l)
    {
      ::DeleteProcThreadAttributeList(l);
      ::HeapFree(GetProcessHeap(), 0, l);
    }
  };
  std::unique_ptr<std::remove_pointer<LPPROC_THREAD_ATTRIBUTE_LIST>::type, lpproc_thread_closer> proc_attribute_list_storage;

  BOOST_PROCESS_V2_DECL LPPROC_THREAD_ATTRIBUTE_LIST get_thread_attribute_list(error_code & ec);
  BOOST_PROCESS_V2_DECL void set_handle_list(error_code & ec);
};


}
BOOST_PROCESS_V2_END_NAMESPACE



#endif //BOOST_PROCESS_V2_WINDOWS_DEFAULT_LAUNCHER_HPP