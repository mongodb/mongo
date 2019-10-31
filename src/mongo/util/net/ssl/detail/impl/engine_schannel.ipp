
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

#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"
#include "mongo/platform/shared_library.h"
#include "mongo/util/net/ssl/detail/engine.hpp"
#include "mongo/util/net/ssl/error.hpp"
#include "mongo/util/text.h"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {
namespace detail {


engine::engine(SCHANNEL_CRED* context, const std::string& remoteHostName)
    : _pCred(context),
      _remoteHostName(mongo::toNativeString(remoteHostName.c_str())),
      _inBuffer(kDefaultBufferSize),
      _outBuffer(kDefaultBufferSize),
      _extraBuffer(kDefaultBufferSize),
      _handshakeManager(
          &_hcxt, &_hcred, _remoteHostName, &_inBuffer, &_outBuffer, &_extraBuffer, _pCred),
      _readManager(&_hcxt, &_hcred, &_inBuffer, &_extraBuffer),
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

    _handshakeManager.setMode((type == asio::ssl::stream_base::client)
                                  ? SSLHandshakeManager::HandshakeMode::Client
                                  : SSLHandshakeManager::HandshakeMode::Server);
    SSLHandshakeManager::HandshakeState state;
    auto w = _handshakeManager.nextHandshake(ec, &state);
    if (w == ssl_want::want_nothing || state == SSLHandshakeManager::HandshakeState::Done) {
        _state = EngineState::InProgress;
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
