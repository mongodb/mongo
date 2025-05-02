// Copyright (c) 2022 Klemens D. Morgenstern (klemens dot morgenstern at gmx dot net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/asio/windows/basic_object_handle.hpp>
#endif

#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/environment.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(BOOST_PROCESS_V2_WINDOWS)

error_code process_environment::on_setup(windows::default_launcher & launcher, const filesystem::path &, const std::wstring &)
{
    if (!unicode_env.empty() && !ec)
    {
      launcher.creation_flags |= CREATE_UNICODE_ENVIRONMENT ;
      launcher.environment = unicode_env.data();
    }

    return ec;
};

#else

error_code process_environment::on_setup(posix::default_launcher & launcher, const filesystem::path &, const char * const *)
{
    launcher.env = env.data();
    return error_code{};
};

#endif


BOOST_PROCESS_V2_END_NAMESPACE
