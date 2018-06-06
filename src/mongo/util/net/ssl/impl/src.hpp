//
// impl/ssl/src.hpp
// ~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_IMPL_SRC_HPP
#define ASIO_SSL_IMPL_SRC_HPP

#define ASIO_SOURCE

#include "asio/detail/config.hpp"

#if defined(ASIO_HEADER_ONLY)
#error Do not compile Asio library source with ASIO_HEADER_ONLY defined
#endif

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS

#include "mongo/util/net/ssl/detail/impl/engine_schannel.ipp"
#include "mongo/util/net/ssl/detail/impl/schannel.ipp"
#include "mongo/util/net/ssl/impl/context_schannel.ipp"
#include "mongo/util/net/ssl/impl/error.ipp"


#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

#include "mongo/util/net/ssl/detail/impl/engine_openssl.ipp"
#include "mongo/util/net/ssl/impl/context_openssl.ipp"
#include "mongo/util/net/ssl/impl/error.ipp"

#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE

#include "mongo/util/net/ssl/detail/impl/engine_apple.ipp"
#include "mongo/util/net/ssl/impl/error.ipp"

#else
#error "Unknown SSL Provider"
#endif


#endif  // ASIO_SSL_IMPL_SRC_HPP
