// Copyright (c) 2022 Klemens D. Morgenstern
// Copyright (c) 2022 Samuel Venable
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_PID_HPP
#define BOOST_PROCESS_V2_PID_HPP

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/throw_error.hpp>

#include <vector>
#include <memory>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(GENERATING_DOCUMENTATION)

//An integral type representing a process id.
typedef implementation_defined pid_type;

#else

#if defined(BOOST_PROCESS_V2_WINDOWS)

typedef unsigned long pid_type;

#else

typedef int pid_type;

#endif
#endif

#if (defined(BOOST_PROCESS_V2_WINDOWS) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__sun))
constexpr static pid_type root_pid = 0;
#elif (defined(__APPLE__) && defined(__MACH__) || defined(__linux__) || defined(__ANDROID__) || defined(__OpenBSD__))
constexpr static pid_type root_pid = 1;
#endif

/// Get the process id of the current process.
BOOST_PROCESS_V2_DECL pid_type current_pid();

/// List all available pids.
BOOST_PROCESS_V2_DECL std::vector<pid_type> all_pids(error_code & ec);

/// List all available pids.
BOOST_PROCESS_V2_DECL std::vector<pid_type> all_pids();

// return parent pid of pid.
BOOST_PROCESS_V2_DECL pid_type parent_pid(pid_type pid, error_code & ec);

// return parent pid of pid.
BOOST_PROCESS_V2_DECL pid_type parent_pid(pid_type pid);

// return child pids of pid.
BOOST_PROCESS_V2_DECL std::vector<pid_type> child_pids(pid_type pid, error_code & ec);

// return child pids of pid.
BOOST_PROCESS_V2_DECL std::vector<pid_type> child_pids(pid_type pid);

BOOST_PROCESS_V2_END_NAMESPACE


#endif // BOOST_PROCESS_V2_PID_HPP

