// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/net/ssl/apple.hpp"
#include "mongo/util/net/ssl/context_apple.hpp"
#include "mongo/util/net/ssl/stream_base.hpp"

#include <deque>

#include <asio/buffer.hpp>
#include <asio/detail/config.hpp>
#include <asio/error_code.hpp>
#include <boost/optional.hpp>

// This must be after all other includes
#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {
namespace detail {

class engine {
public:
    using native_handle_type = ::SSLContextRef;
    enum want {
        want_input_and_retry = -2,
        want_output_and_retry = -1,
        want_nothing = 0,
        want_output = 1
    };

    ASIO_DECL explicit engine(context::native_handle_type context,
                              const std::string& remoteHostName);

    ASIO_DECL native_handle_type native_handle() {
        return _ssl.get();
    }

    ASIO_DECL boost::optional<std::string> get_sni();

    ASIO_DECL want handshake(stream_base::handshake_type type, asio::error_code& ec);

    ASIO_DECL want shutdown(asio::error_code& ec);

    ASIO_DECL want write(const asio::const_buffer& data,
                         asio::error_code& ec,
                         std::size_t& bytes_transferred);

    ASIO_DECL want read(const asio::mutable_buffer& data,
                        asio::error_code& ec,
                        std::size_t& bytes_transferred);

    ASIO_DECL asio::mutable_buffer get_output(const asio::mutable_buffer& data);

    ASIO_DECL asio::const_buffer put_input(const asio::const_buffer& data);

    ASIO_DECL const asio::error_code& map_error_code(asio::error_code& ec) const;

private:
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;
    bool _initSSL(stream_base::handshake_type type, asio::error_code& ec);
    static ::OSStatus read_func(::SSLConnectionRef ctx, void* data, size_t* data_len);
    static ::OSStatus write_func(::SSLConnectionRef ctx, const void* data, size_t* data_len);
    want wouldBlock() const;

    // TLS SNI server name
    std::string _remoteHostName;

    // TLS SNI name received from remote side
    boost::optional<std::string> _sni;

    apple::CFUniquePtr<native_handle_type> _ssl;
    apple::CFUniquePtr<::CFArrayRef> _certs;
    apple::CFUniquePtr<::CFArrayRef> _ca;
    ::SSLProtocol _protoMin, _protoMax;
    std::deque<char> _inbuf;
    std::deque<char> _outbuf;
};

}  // namespace detail
}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
#include "mongo/util/net/ssl/detail/impl/engine_apple.ipp"
#endif  // defined(ASIO_HEADER_ONLY)
