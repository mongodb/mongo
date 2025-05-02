//
// boost/process/v2/windows/default_launcher.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_WINDOWS_CREATION_FLAGS_HPP
#define BOOST_PROCESS_V2_WINDOWS_CREATION_FLAGS_HPP

#include <boost/process/v2/windows/default_launcher.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace windows
{


/// An initializer to add to the dwFlags in the startup-info
/**
 * @tparam Flags The flags to be set.
 */
template<DWORD Flags>
struct process_creation_flags
{
  constexpr process_creation_flags () {}
  
  error_code on_setup(windows::default_launcher & launcher,
                      const filesystem::path &, 
                      const std::wstring &) const
  {
    launcher.creation_flags |= Flags;
    return error_code {};
  };
};



/// A flag to create a new process group. Necessary to allow interrupts for the subprocess.
constexpr static process_creation_flags<CREATE_NEW_PROCESS_GROUP> create_new_process_group;

constexpr static process_creation_flags<CREATE_BREAKAWAY_FROM_JOB> create_breakaway_from_job;
constexpr static process_creation_flags<CREATE_NEW_CONSOLE>        create_new_console;

}
BOOST_PROCESS_V2_END_NAMESPACE

#endif //  BOOST_PROCESS_V2_WINDOWS_CREATION_FLAGS_HPP