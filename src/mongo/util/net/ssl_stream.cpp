//
// asio_ssl.cpp
// ~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "mongo/platform/basic.h"

#include "mongo/config.h"

#include "mongo/util/fail_point_service.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl/impl/src.hpp"

namespace asio {
namespace ssl {
namespace detail {
MONGO_FAIL_POINT_DEFINE(smallTLSReads);
}  // namespce detail
}  // namespce ssl
}  // namespce asio

#endif
