// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POSIX_DETAIL_CLOSE_HANDLES_HPP
#define BOOST_PROCESS_V2_POSIX_DETAIL_CLOSE_HANDLES_HPP

#include <boost/process/v2/detail/config.hpp>
#include <vector>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace posix
{

namespace detail
{

// whitelist must be ordered
BOOST_PROCESS_V2_DECL void close_all(const std::vector<int> & whitelist, 
                                     error_code & ec);

}

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_POSIX_DETAIL_CLOSE_HANDLES_HPP
