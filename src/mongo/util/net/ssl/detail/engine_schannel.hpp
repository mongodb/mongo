// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/net/ssl/detail/schannel.hpp"
#include "mongo/util/net/ssl/stream_base.hpp"

#include "asio/buffer.hpp"
#include "asio/detail/config.hpp"
#include "asio/detail/static_mutex.hpp"

// This must be after all other includes
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
    ASIO_DECL explicit engine(SCH_CREDENTIALS* context, const std::string& remoteHostName);

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
    SCH_CREDENTIALS* _pCred;

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
