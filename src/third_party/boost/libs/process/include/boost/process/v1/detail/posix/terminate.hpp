// Copyright (c) 2006, 2007 Julio M. Merino Vidal
// Copyright (c) 2008 Ilya Sokolov, Boris Schaeling
// Copyright (c) 2009 Boris Schaeling
// Copyright (c) 2010 Felipe Tanus, Boris Schaeling
// Copyright (c) 2011, 2012 Jeff Flinn, Boris Schaeling
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_DETAIL_POSIX_TERMINATE_HPP
#define BOOST_PROCESS_DETAIL_POSIX_TERMINATE_HPP

#include <boost/process/v1/detail/config.hpp>
#include <boost/process/v1/detail/posix/child_handle.hpp>
#include <system_error>
#include <signal.h>
#include <sys/wait.h>


namespace boost { namespace process { BOOST_PROCESS_V1_INLINE namespace v1 { namespace detail { namespace posix {

inline void terminate(const child_handle &p, std::error_code &ec) noexcept
{
    if (::kill(p.pid, SIGKILL) == -1)
        ec = boost::process::v1::detail::get_last_error();
    else
        ec.clear();

    int status;
    ::waitpid(p.pid, &status, 0); //should not be WNOHANG, since that would allow zombies.
}

inline void terminate(const child_handle &p)
{
    std::error_code ec;
    terminate(p, ec);
    boost::process::v1::detail::throw_error(ec, "kill(2) failed");
}

}}}}}

#endif
