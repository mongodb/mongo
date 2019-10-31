
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <deque>

#include "asio/buffer.hpp"
#include "asio/detail/config.hpp"
#include "mongo/util/net/ssl/apple.hpp"
#include "mongo/util/net/ssl/context_apple.hpp"
#include "mongo/util/net/ssl/stream_base.hpp"

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
