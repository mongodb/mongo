// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/config.h"
#include "mongo/util/fail_point.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl/impl/src.hpp"

namespace asio {
namespace ssl {
namespace detail {
MONGO_FAIL_POINT_DEFINE(smallTLSReads);

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS
// SChannel function to get the SNI from the client hello
SslGetServerIdentityFn SSLHandshakeManager::_sslGetServerIdentityFn;
#endif

}  // namespace detail
}  // namespace ssl
}  // namespace asio

#endif
