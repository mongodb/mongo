// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/net/ssl/detail/engine.hpp"
#include "mongo/util/net/ssl/error.hpp"
#include "mongo/util/text.h"

#include "asio/detail/config.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"

// This must be after all other includes
#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {
namespace detail {

// Bring in the _attr UDL (defined in mongo::literals) needed by LOGV2_DEBUG.
using namespace mongo::literals;

engine::engine(SCH_CREDENTIALS* context, const std::string& remoteHostName)
    : _pCred(context),
      _remoteHostName(mongo::toNativeString(remoteHostName.c_str())),
      _inBuffer(kDefaultBufferSize),
      _outBuffer(kDefaultBufferSize),
      _extraBuffer(kDefaultBufferSize),
      _handshakeManager(
          &_hcxt, &_hcred, _remoteHostName, &_inBuffer, &_outBuffer, &_extraBuffer, _pCred),
      _readManager(&_hcxt, &_hcred, &_inBuffer, &_extraBuffer, &_outBuffer, &_remoteHostName),
      _writeManager(&_hcxt, &_outBuffer) {
    SecInvalidateHandle(&_hcxt);
    SecInvalidateHandle(&_hcred);
}

engine::~engine() {
    DeleteSecurityContext(&_hcxt);
    FreeCredentialsHandle(&_hcred);
}

PCtxtHandle engine::native_handle() {
    return &_hcxt;
}

boost::optional<std::string> engine::get_sni() const {
    return _handshakeManager.getSNI();
}

engine::want ssl_want_to_engine(ssl_want want) {
    static_assert(static_cast<int>(ssl_want::want_input_and_retry) ==
                      static_cast<int>(engine::want_input_and_retry),
                  "bad");
    static_assert(static_cast<int>(ssl_want::want_output_and_retry) ==
                      static_cast<int>(engine::want_output_and_retry),
                  "bad");
    static_assert(
        static_cast<int>(ssl_want::want_nothing) == static_cast<int>(engine::want_nothing), "bad");
    static_assert(static_cast<int>(ssl_want::want_output) == static_cast<int>(engine::want_output),
                  "bad");

    return static_cast<engine::want>(want);
}

engine::want engine::handshake(stream_base::handshake_type type, asio::error_code& ec) {
    // ASIO will call handshake once more after we send out the last data
    // so we need to tell them we are done with data to send.
    if (_state != EngineState::NeedsHandshake) {
        return want::want_nothing;
    }

    const bool isClient = (type == asio::ssl::stream_base::client);
    _handshakeManager.setMode(isClient ? SSLHandshakeManager::HandshakeMode::Client
                                       : SSLHandshakeManager::HandshakeMode::Server);
    _readManager.setIsClient(isClient);
    SSLHandshakeManager::HandshakeState state;
    auto w = _handshakeManager.nextHandshake(ec, &state);
    if (!ec &&
        (w == ssl_want::want_nothing || state == SSLHandshakeManager::HandshakeState::Done)) {
        _state = EngineState::InProgress;

        // TLS 1.3: the peer may bundle application data alongside its final handshake
        // flight (e.g. the client's Certificate+Finished + first MongoDB message arrive in
        // one TCP segment).  AcceptSecurityContext / InitializeSecurityContext leaves those
        // encrypted bytes in _inBuffer, but the SSLReadManager is still in its initial
        // NeedMoreEncryptedData state and will stall waiting for more network data.  Signal
        // it so readDecryptedData processes the leftover bytes immediately.
        if (!_inBuffer.empty()) {
            LOGV2_DEBUG(7998008,
                        2,
                        "TLS handshake complete with leftover application data in input buffer",
                        "bytes"_attr = _inBuffer.size());
            _readManager.notifyHandshakeLeftoverData();
        }
    }

    return ssl_want_to_engine(w);
}

engine::want engine::shutdown(asio::error_code& ec) {
    return ssl_want_to_engine(_handshakeManager.beginShutdown(ec));
}

engine::want engine::write(const asio::const_buffer& data,
                           asio::error_code& ec,
                           std::size_t& bytes_transferred) {
    if (data.size() == 0) {
        ec = asio::error_code();
        return engine::want_nothing;
    }

    if (_state == EngineState::NeedsHandshake || _state == EngineState::InShutdown) {
        // Why are we trying to write before the handshake is done?
        ASIO_ASSERT(false);
        return want::want_nothing;
    } else {
        return ssl_want_to_engine(
            _writeManager.writeUnencryptedData(data.data(), data.size(), bytes_transferred, ec));
    }
}

engine::want engine::read(const asio::mutable_buffer& data,
                          asio::error_code& ec,
                          std::size_t& bytes_transferred) {
    if (data.size() == 0) {
        ec = asio::error_code();
        return engine::want_nothing;
    }


    if (_state == EngineState::NeedsHandshake) {
        // Why are we trying to read before the handshake is done?
        ASIO_ASSERT(false);
        return want::want_nothing;
    } else {
        SSLReadManager::DecryptState decryptState;
        auto want = ssl_want_to_engine(_readManager.readDecryptedData(
            data.data(), data.size(), ec, bytes_transferred, &decryptState));
        if (ec) {
            return want;
        }

        if (decryptState != SSLReadManager::DecryptState::Continue) {
            if (decryptState == SSLReadManager::DecryptState::Shutdown) {
                _state = EngineState::InShutdown;

                return ssl_want_to_engine(_handshakeManager.beginShutdown(ec));
            }
        }

        return want;
    }
}

asio::mutable_buffer engine::get_output(const asio::mutable_buffer& data) {
    std::size_t length;
    _outBuffer.readInto(data.data(), data.size(), length);

    return asio::buffer(data, length);
}

asio::const_buffer engine::put_input(const asio::const_buffer& data) {
    if (_state == EngineState::NeedsHandshake) {
        _handshakeManager.writeEncryptedData(data.data(), data.size());
    } else {
        _readManager.writeData(data.data(), data.size());
    }

    return asio::buffer(data + data.size());
}

const asio::error_code& engine::map_error_code(asio::error_code& ec) const {
    return ec;
}

#include "asio/detail/pop_options.hpp"

}  // namespace detail
}  // namespace ssl
}  // namespace asio
