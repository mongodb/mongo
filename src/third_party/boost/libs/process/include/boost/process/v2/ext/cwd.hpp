// Copyright (c) 2022 Klemens D. Morgenstern
// Copyright (c) 2022 Samuel Venable
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_CWD_HPP
#define BOOST_PROCESS_V2_CWD_HPP

#include <boost/process/v2/detail/config.hpp>

#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process_handle.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace ext {

#if defined(BOOST_PROCESS_V2_WINDOWS)
BOOST_PROCESS_V2_DECL filesystem::path cwd(HANDLE handle, error_code & ec);
BOOST_PROCESS_V2_DECL filesystem::path cwd(HANDLE handle);
#endif

/// @{
/// Obtain the current path of another process
BOOST_PROCESS_V2_DECL filesystem::path cwd(pid_type pid, error_code & ec);
BOOST_PROCESS_V2_DECL filesystem::path cwd(pid_type pid);

template<typename Executor>
inline filesystem::path cwd(basic_process_handle<Executor> & handle, error_code & ec)
{
#if defined(BOOST_PROCESS_V2_WINDOWS)
    return cwd(handle.native_handle(), ec);
#else
    return cwd(handle.id(), ec);
#endif
}

template<typename Executor>
inline filesystem::path cwd(basic_process_handle<Executor> & handle)
{
#if defined(BOOST_PROCESS_V2_WINDOWS)
    return cwd(handle.native_handle());
#else
    return cwd(handle.id());
#endif
}

/// @}

} // namespace ext

BOOST_PROCESS_V2_END_NAMESPACE

#endif // BOOST_PROCESS_V2_CWD_HPP
