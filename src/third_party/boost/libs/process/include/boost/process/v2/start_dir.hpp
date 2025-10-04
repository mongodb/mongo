//
// process/start_dir.hpp
// ~~~~~~~~
//
// Copyright (c) 2021 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_v2_START_DIR_HPP
#define BOOST_PROCESS_v2_START_DIR_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

/// Initializer for the starting directory of a subprocess to be launched.
struct process_start_dir
{
  filesystem::path start_dir;

  process_start_dir(filesystem::path start_dir) : start_dir(std::move(start_dir))
  {
  }

#if defined(BOOST_PROCESS_V2_WINDOWS)
  error_code on_setup(windows::default_launcher & launcher, 
                      const filesystem::path &, const std::wstring &)
  {
    launcher.current_directory = start_dir;
    return error_code {};
  };

#else
  error_code on_exec_setup(posix::default_launcher & /*launcher*/,
                           const filesystem::path &, const char * const *)
  {
    if (::chdir(start_dir.c_str()) == -1)
      return detail::get_last_error();
    else
      return error_code ();
  }
#endif

};

BOOST_PROCESS_V2_END_NAMESPACE

#endif //  BOOST_PROCESS_v2_START_DIR_HPP