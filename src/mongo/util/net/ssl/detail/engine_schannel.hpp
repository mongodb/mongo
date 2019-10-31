
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

#include "asio/detail/config.hpp"

#include "asio/buffer.hpp"
#include "asio/detail/static_mutex.hpp"
#include "mongo/util/net/ssl/detail/schannel.hpp"
#include "mongo/util/net/ssl/stream_base.hpp"

#include "asio/detail/push_options.hpp"

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
    ASIO_DECL explicit engine(SCHANNEL_CRED* context, const std::string& remoteHostName);

    // Destructor.
    ASIO_DECL ~engine();

    // Get the underlying implementation in the native type.
    ASIO_DECL PCtxtHandle native_handle();

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

    // Returns the SNI from the handshake manager.
    boost::optional<std::string> get_sni() const;

private:
    // Disallow copying and assignment.
    engine(const engine&);
    engine& operator=(const engine&);

private:
    // SChannel context handle
    CtxtHandle _hcxt;

    // Credential handle
    CredHandle _hcred;

    // Credentials for TLS handshake
    SCHANNEL_CRED* _pCred;

    // TLS SNI server name
    std::wstring _remoteHostName;

    // Engine State machine
    //
    enum class EngineState {
        // Initial State
        NeedsHandshake,

        // Normal SSL Conversation in progress
        InProgress,

        // In SSL shutdown
        InShutdown,
    };

    // Engine state
    EngineState _state{EngineState::NeedsHandshake};

    // Data received from remote side, shared across state machines
    ReusableBuffer _inBuffer;

    // Data to send to remote side, shared across state machines
    ReusableBuffer _outBuffer;

    // Extra buffer - for when more then one packet is read from the remote side
    ReusableBuffer _extraBuffer;

    // Handshake state machine
    SSLHandshakeManager _handshakeManager;

    // Read state machine
    SSLReadManager _readManager;

    // Write state machine
    SSLWriteManager _writeManager;
};

#include "asio/detail/pop_options.hpp"

}  // namespace detail
}  // namespace ssl
}  // namespace asio
