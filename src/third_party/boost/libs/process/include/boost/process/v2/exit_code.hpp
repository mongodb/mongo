//
// process/exit_code.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_EXIT_CODE_HPP
#define BOOST_PROCESS_V2_EXIT_CODE_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/error.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/associator.hpp>
#include <asio/async_result.hpp>
#else
#include <boost/asio/associator.hpp>
#include <boost/asio/async_result.hpp>
#endif 

#if defined(BOOST_PROCESS_V2_POSIX)
#include <sys/wait.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(GENERATING_DOCUMENTATION)

/// The native exit-code type, usually an integral value
/** The OS may have a value different from `int` to represent 
 * the exit codes of subprocesses. It might also 
 * contain additional information.
 */ 
typedef implementation_defined native_exit_code_type;


/// Check if the native exit code indicates the process is still running
bool process_is_running(native_exit_code_type code);

/// Obtain the portable part of the exit code, i.e. what the subprocess has returned from main.
int evaluate_exit_code(native_exit_code_type code);


#else

#if defined(BOOST_PROCESS_V2_WINDOWS)

typedef unsigned long native_exit_code_type;

namespace detail
{
constexpr native_exit_code_type still_active = 259u;
}

inline bool process_is_running(native_exit_code_type code)
{
  return code == detail::still_active;
}

inline int evaluate_exit_code(native_exit_code_type code)
{
  return static_cast<int>(code);
}

#else

typedef int native_exit_code_type;

namespace detail
{
constexpr native_exit_code_type still_active = 0x17f;
static_assert(WIFSTOPPED(still_active), "Expected still_active to indicate WIFSTOPPED");
static_assert(!WIFEXITED(still_active), "Expected still_active to not indicate WIFEXITED");
static_assert(!WIFSIGNALED(still_active), "Expected still_active to not indicate WIFSIGNALED");
static_assert(!WIFCONTINUED(still_active), "Expected still_active to not indicate WIFCONTINUED");
}

inline bool process_is_running(int code)
{
    return !WIFEXITED(code) && !WIFSIGNALED(code);
}

inline int evaluate_exit_code(int code)
{
  if (WIFEXITED(code))
    return WEXITSTATUS(code);
  else if (WIFSIGNALED(code))
    return WTERMSIG(code);
  else
    return code;
}

#endif

#endif

/// @{
/** Helper to subsume an exit-code into an error_code if there's no actual error isn't set.
 * @code {.cpp}
 * process proc{ctx, "exit", {"1"}};
 * 
 * proc.async_wait(
 *     asio::deferred(
 *      [&proc](error_code ec, int)
 *      {
 *        return asio::deferred.values(
 *                  check_exit_code(ec, proc.native_exit_code())
 *              ))
 *      [](error_code ec)
 *      {
 *        assert(ec.value() == 10);
 *        assert(ec.category() == error::get_exit_code_category());
 *      }));
 * 
 * @endcode
 */

inline error_code check_exit_code(
    error_code &ec, native_exit_code_type native_code,
    const error_category & category = error::get_exit_code_category())
{
  if (!ec)
    BOOST_PROCESS_V2_ASSIGN_EC(ec, native_code, category);
  return ec;
}

/// @}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_EXIT_CODE_HPP