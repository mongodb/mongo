//
// ssl/detail/engine.hpp
// ~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_DETAIL_ENGINE_OPENSSL_HPP
#define ASIO_SSL_DETAIL_ENGINE_OPENSSL_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "mongo/util/net/ssl/detail/openssl_types.hpp"
#include "mongo/util/net/ssl/stream_base.hpp"

#include <asio/buffer.hpp>
#include <asio/detail/config.hpp>
#include <asio/detail/static_mutex.hpp>
#include <asio/error_code.hpp>
#include <boost/optional.hpp>

// This must be after all other includes
#include <asio/detail/push_options.hpp>

namespace asio {
namespace ssl {
namespace detail {

class engine {
public:
    enum want {
        // Returned by functions to indicate that the engine wants input. The input
        // buffer should be updated to point to the data. The engine then needs to
        // be called again to retry the operation.
        want_input_and_retry = -2,

        // Returned by functions to indicate that the engine wants to write output.
        // The output buffer points to the data to be written. The engine then
        // needs to be called again to retry the operation.
        want_output_and_retry = -1,

        // Returned by functions to indicate that the engine doesn't need input or
        // output.
        want_nothing = 0,

        // Returned by functions to indicate that the engine wants to write output.
        // The output buffer points to the data to be written. After that the
        // operation is complete, and the engine does not need to be called again.
        want_output = 1
    };

    // Construct a new engine for the specified context.
    ASIO_DECL explicit engine(SSL_CTX* context, const std::string& remoteHostName);

    // Destructor.
    ASIO_DECL ~engine();

    // Get the underlying implementation in the native type.
    ASIO_DECL SSL* native_handle();

    boost::optional<std::string> get_sni();

    // Perform an SSL handshake using either SSL_connect (client-side) or
    // SSL_accept (server-side).
    ASIO_DECL want handshake(stream_base::handshake_type type, asio::error_code& ec);

    // Perform a graceful shutdown of the SSL session.
    ASIO_DECL want shutdown(asio::error_code& ec);

    // Write bytes to the SSL session.
    ASIO_DECL want write(const asio::const_buffer& data,
                         asio::error_code& ec,
                         std::size_t& bytes_transferred);

    // Read bytes from the SSL session.
    ASIO_DECL want read(const asio::mutable_buffer& data,
                        asio::error_code& ec,
                        std::size_t& bytes_transferred);

    // Get output data to be written to the transport.
    ASIO_DECL asio::mutable_buffer get_output(const asio::mutable_buffer& data);

    // Put input data that was read from the transport.
    ASIO_DECL asio::const_buffer put_input(const asio::const_buffer& data);

    // Map an error::eof code returned by the underlying transport according to
    // the type and state of the SSL session. Returns a const reference to the
    // error code object, suitable for passing to a completion handler.
    ASIO_DECL const asio::error_code& map_error_code(asio::error_code& ec) const;

private:
    // Disallow copying and assignment.
    engine(const engine&);
    engine& operator=(const engine&);

#if (OPENSSL_VERSION_NUMBER < 0x10000000L)
    // The SSL_accept function may not be thread safe. This mutex is used to
    // protect all calls to the SSL_accept function.
    ASIO_DECL static asio::detail::static_mutex& accept_mutex();
#endif  // (OPENSSL_VERSION_NUMBER < 0x10000000L)

    ASIO_DECL void purge_error_state();

    // Perform one operation. Returns >= 0 on success or error, want_read if the
    // operation needs more input, or want_write if it needs to write some output
    // before the operation can complete.
    ASIO_DECL want perform(int (engine::*op)(void*, std::size_t),
                           void* data,
                           std::size_t length,
                           asio::error_code& ec,
                           std::size_t* bytes_transferred);

    // Adapt the SSL_accept function to the signature needed for perform().
    ASIO_DECL int do_accept(void*, std::size_t);

    // Adapt the SSL_connect function to the signature needed for perform().
    ASIO_DECL int do_connect(void*, std::size_t);

    // Adapt the SSL_shutdown function to the signature needed for perform().
    ASIO_DECL int do_shutdown(void*, std::size_t);

    // Adapt the SSL_read function to the signature needed for perform().
    ASIO_DECL int do_read(void* data, std::size_t length);

    // Adapt the SSL_write function to the signature needed for perform().
    ASIO_DECL int do_write(void* data, std::size_t length);

    SSL* ssl_;
    BIO* ext_bio_;

    // TLS SNI server name
    std::string _remoteHostName;

    // TLS SNI name expected by incoming client
    boost::optional<std::string> _sni;
};

}  // namespace detail
}  // namespace ssl
}  // namespace asio

#include <asio/detail/pop_options.hpp>

#if defined(ASIO_HEADER_ONLY)
#include "mongo/util/net/ssl/detail/impl/engine_openssl.ipp"
#endif  // defined(ASIO_HEADER_ONLY)

#endif  // ASIO_SSL_DETAIL_ENGINE_OPENSSL_HPP
