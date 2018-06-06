//
// ssl/context_base.hpp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_CONTEXT_BASE_HPP
#define ASIO_SSL_CONTEXT_BASE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include "mongo/util/net/ssl/detail/openssl_types.hpp"
#endif

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

/// The context_base class is used as a base for the basic_context class
/// template so that we have a common place to define various enums.
class context_base {
public:
    /// Different methods supported by a context.
    enum method {
        /// Generic SSL version 2.
        sslv2,

        /// SSL version 2 client.
        sslv2_client,

        /// SSL version 2 server.
        sslv2_server,

        /// Generic SSL version 3.
        sslv3,

        /// SSL version 3 client.
        sslv3_client,

        /// SSL version 3 server.
        sslv3_server,

        /// Generic TLS version 1.
        tlsv1,

        /// TLS version 1 client.
        tlsv1_client,

        /// TLS version 1 server.
        tlsv1_server,

        /// Generic SSL/TLS.
        sslv23,

        /// SSL/TLS client.
        sslv23_client,

        /// SSL/TLS server.
        sslv23_server,

        /// Generic TLS version 1.1.
        tlsv11,

        /// TLS version 1.1 client.
        tlsv11_client,

        /// TLS version 1.1 server.
        tlsv11_server,

        /// Generic TLS version 1.2.
        tlsv12,

        /// TLS version 1.2 client.
        tlsv12_client,

        /// TLS version 1.2 server.
        tlsv12_server,

        /// Generic TLS.
        tls,

        /// TLS client.
        tls_client,

        /// TLS server.
        tls_server
    };

    /// Bitmask type for SSL options.
    typedef long options;

protected:
    /// Protected destructor to prevent deletion through this type.
    ~context_base() {}
};

}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"

#endif  // ASIO_SSL_CONTEXT_BASE_HPP
