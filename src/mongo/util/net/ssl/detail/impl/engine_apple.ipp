
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "asio/detail/config.hpp"

#include "asio/detail/push_options.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl/apple.hpp"
#include "mongo/util/net/ssl/detail/engine.hpp"
#include "mongo/util/net/ssl/detail/stream_core.hpp"
#include "mongo/util/net/ssl/error.hpp"

namespace asio {
namespace ssl {
namespace detail {

namespace {
// Limit size of output buffer to avoid growing indefinitely.
constexpr auto max_outbuf_size = stream_core::max_tls_record_size;

const class osstatus_category : public error_category {
public:
    const char* name() const noexcept final {
        return "Secure.Transport";
    }

    std::string message(int value) const noexcept final {
        const auto status = static_cast<::OSStatus>(value);
        apple::CFUniquePtr<::CFStringRef> errstr(::SecCopyErrorMessageString(status, nullptr));
        if (!errstr) {
            return mongo::str::stream() << "Secure.Transport unknown error: "
                                        << static_cast<int>(status);
        }
        const auto len = ::CFStringGetMaximumSizeForEncoding(::CFStringGetLength(errstr.get()),
                                                             ::kCFStringEncodingUTF8);
        std::string ret;
        ret.resize(len + 1);
        if (!::CFStringGetCString(errstr.get(), &ret[0], len, ::kCFStringEncodingUTF8)) {
            return mongo::str::stream() << "Secure.Transport unknown error: "
                                        << static_cast<int>(status);
        }

        ret.resize(strlen(ret.c_str()));
        return mongo::str::stream() << "Secure.Transport: " << ret;
    }
} OSStatus_category;

asio::error_code errorCode(::OSStatus status) {
    return asio::error_code(static_cast<int>(status), OSStatus_category);
}

/**
 * Verify that an SSL session is ready for I/O (state: Connected).
 * In all other states, asio should be speaking to the socket directly.
 */
bool verifyConnected(::SSLContextRef ssl, asio::error_code* ec) {
    auto state = ::kSSLAborted;
    auto status = ::SSLGetSessionState(ssl, &state);
    if (status != ::errSecSuccess) {
        // Unable to determine session state.
        *ec = errorCode(status);
        return false;
    }
    switch (state) {
        case ::kSSLIdle:
            *ec = asio::error::not_connected;
            return false;
        case ::kSSLHandshake:
            *ec = asio::error::in_progress;
            return false;
        case ::kSSLConnected:
            return true;
        case ::kSSLClosed:
            *ec = asio::error::shut_down;
            return false;
        case ::kSSLAborted:
            *ec = asio::error::connection_aborted;
            return false;
        default:
            // Undefined state, call it an internal error.
            *ec = errorCode(::errSSLInternal);
            return false;
    }
}

}  // namespace

engine::engine(context::native_handle_type context, const std::string& remoteHostName)
    : _remoteHostName(remoteHostName) {
    if (context) {
        if (context->certs) {
            ::CFRetain(context->certs.get());
            _certs.reset(context->certs.get());
        }
        _protoMin = context->protoMin;
        _protoMax = context->protoMax;
        if (context->allowInvalidHostnames) {
            _remoteHostName.clear();
        }
    } else {
        apple::Context def;
        _protoMin = def.protoMin;
        _protoMax = def.protoMax;
    }
}

bool engine::_initSSL(stream_base::handshake_type type, asio::error_code& ec) {
    if (_ssl) {
        return true;
    }

    const auto side = (type == stream_base::client) ? ::kSSLClientSide : ::kSSLServerSide;
    _ssl.reset(::SSLCreateContext(nullptr, side, ::kSSLStreamType));
    if (!_ssl) {
        mongo::error() << "Failed allocating SSLContext";
        ec = errorCode(::errSSLInternal);
        return false;
    }

    auto status = ::SSLSetConnection(_ssl.get(), static_cast<void*>(this));

    if (_certs && (status == ::errSecSuccess)) {
        status = ::SSLSetCertificate(_ssl.get(), _certs.get());
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetPeerID(_ssl.get(), _ssl.get(), sizeof(native_handle_type));
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetIOFuncs(_ssl.get(), read_func, write_func);
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetProtocolVersionMin(_ssl.get(), _protoMin);
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetProtocolVersionMax(_ssl.get(), _protoMax);
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetClientSideAuthenticate(_ssl.get(), ::kTryAuthenticate);
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetSessionOption(_ssl.get(), ::kSSLSessionOptionBreakOnServerAuth, true);
    }

    if (status == ::errSecSuccess) {
        status = ::SSLSetSessionOption(_ssl.get(), ::kSSLSessionOptionBreakOnClientAuth, true);
    }

    if (!_remoteHostName.empty() && (status == ::errSecSuccess)) {
        status =
            ::SSLSetPeerDomainName(_ssl.get(), _remoteHostName.c_str(), _remoteHostName.size());
    }

    if (status != ::errSecSuccess) {
        _ssl.reset(nullptr);
        ec = errorCode(status);
        return false;
    }

    return true;
}

engine::want engine::handshake(stream_base::handshake_type type, asio::error_code& ec) {
    ec = asio::error_code();
    if (!_initSSL(type, ec)) {
        // Error happened, ec has been set.
        return want::want_nothing;
    }

    // We use BreakOnClientAuth and BreakOnServerAuth above to
    // convince the OS not to validate the certs for us.
    // In practice, we'll be validating the peer in ssl_manager_apple.cpp later.
    // As a side effect, we have to call SSLHandshake up to three times.
    // Breaking once for client auth, then for server auth, and finally on completion.
    ::OSStatus status;
    do {
        status = ::SSLHandshake(_ssl.get());
    } while ((status == ::errSSLServerAuthCompleted) || (status == ::errSSLClientAuthCompleted));

    if (status == ::errSSLWouldBlock) {
        return wouldBlock();
    }

    if (status != ::errSecSuccess) {
        _ssl.reset(nullptr);
        ec = errorCode(status);
        return want::want_nothing;
    }

    return _outbuf.size() ? want::want_output : want::want_nothing;
}

engine::want engine::shutdown(asio::error_code& ec) {
    ec = asio::error_code();
    if (_ssl) {
        const auto status = ::SSLClose(_ssl.get());
        if (status == ::errSSLWouldBlock) {
            return wouldBlock();
        }
        if (status == ::errSecSuccess) {
            _ssl.reset(nullptr);
        } else {
            ec = errorCode(status);
        }
    } else {
        mongo::error() << "SSL connection already shut down";
        ec = errorCode(::errSSLInternal);
    }
    return want::want_nothing;
}

const asio::error_code& engine::map_error_code(asio::error_code& ec) const {
    if (ec != asio::error::eof) {
        return ec;
    }

    if (_inbuf.size() || _outbuf.size()) {
        ec = asio::ssl::error::stream_truncated;
        return ec;
    }

    invariant(_ssl);
    auto state = ::kSSLAborted;
    const auto status = ::SSLGetSessionState(_ssl.get(), &state);
    if (status != ::errSecSuccess) {
        ec = errorCode(status);
        return ec;
    }

    if (state == ::kSSLConnected) {
        ec = asio::ssl::error::stream_truncated;
        return ec;
    }

    return ec;
}

engine::want engine::write(const asio::const_buffer& data,
                           asio::error_code& ec,
                           std::size_t& bytes_transferred) {
    ec = asio::error_code();
    if (!verifyConnected(_ssl.get(), &ec)) {
        return want::want_nothing;
    }
    const auto status = ::SSLWrite(_ssl.get(), data.data(), data.size(), &bytes_transferred);
    if (status == ::errSSLWouldBlock) {
        return (bytes_transferred < data.size()) ? want::want_output_and_retry : want::want_nothing;
    }
    if (status != ::errSecSuccess) {
        ec = errorCode(status);
    }
    return _outbuf.size() ? want::want_output : want::want_nothing;
}

asio::mutable_buffer engine::get_output(const asio::mutable_buffer& data) {
    const auto len = std::min<size_t>(data.size(), _outbuf.size());
    if (len > 0) {
        auto* p = const_cast<char*>(static_cast<const char*>(data.data()));
        std::copy(_outbuf.begin(), _outbuf.begin() + len, p);
        _outbuf.erase(_outbuf.begin(), _outbuf.begin() + len);
    }

    return asio::mutable_buffer(data.data(), len);
}

::OSStatus engine::write_func(::SSLConnectionRef ctx, const void* data, size_t* data_len) {
    auto* this_ = const_cast<engine*>(static_cast<const engine*>(ctx));
    const auto* p = static_cast<const char*>(data);

    const auto requested = *data_len;
    *data_len = std::min<size_t>(requested, max_outbuf_size - this_->_outbuf.size());
    this_->_outbuf.insert(this_->_outbuf.end(), p, p + *data_len);
    return (requested == *data_len) ? ::errSecSuccess : ::errSSLWouldBlock;
}

engine::want engine::read(const asio::mutable_buffer& data,
                          asio::error_code& ec,
                          std::size_t& bytes_transferred) {
    ec = asio::error_code();
    if (!verifyConnected(_ssl.get(), &ec)) {
        return want::want_nothing;
    }
    const auto status = ::SSLRead(_ssl.get(), data.data(), data.size(), &bytes_transferred);
    if ((status != ::errSSLWouldBlock) && (status != ::errSecSuccess)) {
        ec = errorCode(status);
    }
    return bytes_transferred ? want::want_nothing : wouldBlock();
}

asio::const_buffer engine::put_input(const asio::const_buffer& data) {
    const auto* p = static_cast<const char*>(data.data());
    _inbuf.insert(_inbuf.end(), p, p + data.size());
    return asio::buffer(data + data.size());
}

::OSStatus engine::read_func(::SSLConnectionRef ctx, void* data, size_t* data_len) {
    ::OSStatus ret = ::errSecSuccess;
    auto* this_ = const_cast<engine*>(static_cast<const engine*>(ctx));
    if (*data_len > this_->_inbuf.size()) {
        // If we're able to 100% satisfy the read request Secure Transport made,
        // then we should ultimately signal that the read is incomplete.
        ret = ::errSSLWouldBlock;
    }
    *data_len = std::min<size_t>(*data_len, this_->_inbuf.size());
    if (*data_len > 0) {
        std::copy(
            this_->_inbuf.begin(), this_->_inbuf.begin() + *data_len, static_cast<char*>(data));
        this_->_inbuf.erase(this_->_inbuf.begin(), this_->_inbuf.begin() + *data_len);
    }
    return ret;
}

engine::want engine::wouldBlock() const {
    return _outbuf.empty() ? want::want_input_and_retry : want::want_output_and_retry;
}

#include "asio/detail/pop_options.hpp"

}  // namespace detail
}  // namespace ssl
}  // namespace asio
