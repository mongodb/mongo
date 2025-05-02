// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_THROW_EXCEPTION_HPP
#define BOOST_PROCESS_V2_DETAIL_THROW_EXCEPTION_HPP

#include <boost/process/v2/detail/config.hpp>

#if !defined(BOOST_PROCESS_V2_STANDALONE)

#include <boost/throw_exception.hpp>

#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

#if defined(BOOST_PROCESS_V2_STANDALONE)

template <typename Exception>
inline void throw_exception(const Exception& e)
{
    throw e;
}

#else

using boost::throw_exception;

#endif

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_DETAIL_THROW_EXCEPTION_HPP
