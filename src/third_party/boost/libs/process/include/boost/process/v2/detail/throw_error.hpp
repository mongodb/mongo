// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_THROW_ERROR_HPP
#define BOOST_PROCESS_V2_DETAIL_THROW_ERROR_HPP

#include <boost/process/v2/detail/config.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace detail
{

BOOST_PROCESS_V2_DECL void do_throw_error(const error_code& err);
BOOST_PROCESS_V2_DECL void do_throw_error(const error_code& err, const char* location);

inline void throw_error(const error_code& err)
{
    if (err)
        do_throw_error(err);
}

inline void throw_error(const error_code& err, const char* location)
{
    if (err)
        do_throw_error(err, location);
}

}
BOOST_PROCESS_V2_END_NAMESPACE


#endif //BOOST_PROCESS_V2_DETAIL_THROW_ERROR_HPP
