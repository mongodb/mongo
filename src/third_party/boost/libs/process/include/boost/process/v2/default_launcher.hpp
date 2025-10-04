//
// boost/process/v2/default_launcher.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_PROCESS_V2_DEFAULT_LAUNCHER_HPP
#define BOOST_PROCESS_V2_DEFAULT_LAUNCHER_HPP

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/process/v2/windows/default_launcher.hpp>
#else
#if defined(BOOST_PROCESS_V2_PDFORK)
#include <boost/process/v2/posix/pdfork_launcher.hpp>
#else
#include <boost/process/v2/posix/default_launcher.hpp>
#endif

#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(GENERATING_DOCUMENTATION)

/// The default launcher for processes.
/** This launcher will be used by process if a 
 * process is launched through the constructor:
 * 
 * @code {.cpp}
 * process proc("test", {});
 * // equivalent to
 * process prod = default_launcher()("test", {});
 * @endcode
 * 
 */

typedef implementation_defined default_process_launcher;

#else
#if defined(BOOST_PROCESS_V2_WINDOWS)
typedef windows::default_launcher default_process_launcher;
#else
#if defined(BOOST_PROCESS_V2_PDFORK)
typedef posix::pdfork_launcher default_process_launcher;
#else
typedef posix::default_launcher default_process_launcher;
#endif
#endif


#endif


BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_DEFAULT_LAUNCHER_HPP