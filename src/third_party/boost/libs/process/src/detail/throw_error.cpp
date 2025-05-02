// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/detail/throw_exception.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace detail
{

void do_throw_error(const error_code& err)
{
    system_error e(err);
    throw_exception(e);
}

void do_throw_error(const error_code& err, const char* location)
{
    system_error e(err, location);
    throw_exception(e);
}

}
BOOST_PROCESS_V2_END_NAMESPACE
