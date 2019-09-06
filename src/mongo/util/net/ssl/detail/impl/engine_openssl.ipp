//
// ssl/detail/impl/engine.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_DETAIL_IMPL_ENGINE_OPENSSL_IPP
#define ASIO_SSL_DETAIL_IMPL_ENGINE_OPENSSL_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/ip/address.hpp"

#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"
#include "mongo/util/net/ssl/detail/engine.hpp"
#include "mongo/util/net/ssl/error.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {
namespace detail {

engine::engine(SSL_CTX* context, const std::string& remoteHostName)
    : ssl_(::SSL_new(context)), _remoteHostName(remoteHostName) {
    if (!ssl_) {
        asio::error_code ec(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        asio::detail::throw_error(ec, "engine");
    }

#if (OPENSSL_VERSION_NUMBER < 0x10000000L)
    accept_mutex().init();
#endif  // (OPENSSL_VERSION_NUMBER < 0x10000000L)

    ::SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
    ::SSL_set_mode(ssl_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    ::BIO* int_bio = nullptr;
    ::BIO_new_bio_pair(&int_bio, 0, &ext_bio_, 0);
    ::SSL_set_bio(ssl_, int_bio, int_bio);
}

engine::~engine() {
    if (SSL_get_app_data(ssl_)) {
        SSL_set_app_data(ssl_, nullptr);
    }

    ::BIO_free(ext_bio_);
    ::SSL_free(ssl_);
}

SSL* engine::native_handle() {
    return ssl_;
}

engine::want engine::handshake(stream_base::handshake_type type, asio::error_code& ec) {
    return perform((type == asio::ssl::stream_base::client) ? &engine::do_connect
                                                            : &engine::do_accept,
                   nullptr,
                   0,
                   ec,
                   nullptr);
}

engine::want engine::shutdown(asio::error_code& ec) {
    return perform(&engine::do_shutdown, nullptr, 0, ec, nullptr);
}

engine::want engine::write(const asio::const_buffer& data,
                           asio::error_code& ec,
                           std::size_t& bytes_transferred) {
    if (data.size() == 0) {
        ec = asio::error_code();
        return engine::want_nothing;
    }

    return perform(
        &engine::do_write, const_cast<void*>(data.data()), data.size(), ec, &bytes_transferred);
}

engine::want engine::read(const asio::mutable_buffer& data,
                          asio::error_code& ec,
                          std::size_t& bytes_transferred) {
    if (data.size() == 0) {
        ec = asio::error_code();
        return engine::want_nothing;
    }

    return perform(&engine::do_read, data.data(), data.size(), ec, &bytes_transferred);
}

asio::mutable_buffer engine::get_output(const asio::mutable_buffer& data) {
    int length = ::BIO_read(ext_bio_, data.data(), static_cast<int>(data.size()));

    return asio::buffer(data, length > 0 ? static_cast<std::size_t>(length) : 0);
}

asio::const_buffer engine::put_input(const asio::const_buffer& data) {
    int length = ::BIO_write(ext_bio_, data.data(), static_cast<int>(data.size()));

    return asio::buffer(data + (length > 0 ? static_cast<std::size_t>(length) : 0));
}

const asio::error_code& engine::map_error_code(asio::error_code& ec) const {
    // We only want to map the error::eof code.
    if (ec != asio::error::eof)
        return ec;

    // If there's data yet to be read, it's an error.
    if (BIO_wpending(ext_bio_)) {
        ec = asio::ssl::error::stream_truncated;
        return ec;
    }

// SSL v2 doesn't provide a protocol-level shutdown, so an eof on the
// underlying transport is passed through.
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    if (SSL_version(ssl_) == SSL2_VERSION)
        return ec;
#endif  // (OPENSSL_VERSION_NUMBER < 0x10100000L)

    // Otherwise, the peer should have negotiated a proper shutdown.
    if ((::SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN) == 0) {
        ec = asio::ssl::error::stream_truncated;
    }

    return ec;
}

#if (OPENSSL_VERSION_NUMBER < 0x10000000L)
asio::detail::static_mutex& engine::accept_mutex() {
    static asio::detail::static_mutex mutex = ASIO_STATIC_MUTEX_INIT;
    return mutex;
}
#endif  // (OPENSSL_VERSION_NUMBER < 0x10000000L)


void engine::purge_error_state() {
#if (OPENSSL_VERSION_NUMBER < 0x1010000fL)
    // OpenSSL 1.1.0 introduced a thread local state storage mechanism.
    // Versions prior sometimes had contention issues on global mutexes
    // which protected thread local state.
    // If we are compiled against a version without native thread local
    // support, cache a pointer to this thread's error state, which we can
    // access without contention. If that state requires no cleanup,
    // we can avoid invoking OpenSSL's more expensive machinery.
    const static thread_local ERR_STATE* es = ERR_get_state();
    if (es->bottom == es->top) {
        return;
    }
#endif  // (OPENSSL_VERSION_NUMBER < 0x1010000fL)
    ::ERR_clear_error();
}

engine::want engine::perform(int (engine::*op)(void*, std::size_t),
                             void* data,
                             std::size_t length,
                             asio::error_code& ec,
                             std::size_t* bytes_transferred) {
    std::size_t pending_output_before = ::BIO_ctrl_pending(ext_bio_);
    purge_error_state();
    int result = (this->*op)(data, length);
    int ssl_error = ::SSL_get_error(ssl_, result);
    int sys_error = static_cast<int>(::ERR_get_error());
    std::size_t pending_output_after = ::BIO_ctrl_pending(ext_bio_);

    if (ssl_error == SSL_ERROR_SSL) {
        ec = asio::error_code(sys_error, asio::error::get_ssl_category());
        return want_nothing;
    }

    if (ssl_error == SSL_ERROR_SYSCALL) {
        ec = asio::error_code(sys_error, asio::error::get_system_category());
        return want_nothing;
    }

    if (result > 0 && bytes_transferred)
        *bytes_transferred = static_cast<std::size_t>(result);

    if (ssl_error == SSL_ERROR_WANT_WRITE) {
        ec = asio::error_code();
        return want_output_and_retry;
    } else if (pending_output_after > pending_output_before) {
        ec = asio::error_code();
        return result > 0 ? want_output : want_output_and_retry;
    } else if (ssl_error == SSL_ERROR_WANT_READ) {
        ec = asio::error_code();
        return want_input_and_retry;
    } else if (::SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN) {
        ec = asio::error::eof;
        return want_nothing;
    } else {
        ec = asio::error_code();
        return want_nothing;
    }
}

int engine::do_accept(void*, std::size_t) {
#if (OPENSSL_VERSION_NUMBER < 0x10000000L)
    asio::detail::static_mutex::scoped_lock lock(accept_mutex());
#endif  // (OPENSSL_VERSION_NUMBER < 0x10000000L)
    return ::SSL_accept(ssl_);
}

int engine::do_connect(void*, std::size_t) {
    if (!_remoteHostName.empty()) {
        error_code ec;
        ip::make_address(_remoteHostName, ec);
        // only have TLS advertise _remoteHostName as an SNI if it is not an IP address
        if (ec) {
            int ret = ::SSL_set_tlsext_host_name(ssl_, _remoteHostName.c_str());
            if (ret != 1) {
                return ret;
            }
        }

        _remoteHostName.clear();
    }

    return ::SSL_connect(ssl_);
}

int engine::do_shutdown(void*, std::size_t) {
    int result = ::SSL_shutdown(ssl_);
    if (result == 0)
        result = ::SSL_shutdown(ssl_);
    return result;
}

int engine::do_read(void* data, std::size_t length) {
    return ::SSL_read(ssl_, data, length < INT_MAX ? static_cast<int>(length) : INT_MAX);
}

int engine::do_write(void* data, std::size_t length) {
    return ::SSL_write(ssl_, data, length < INT_MAX ? static_cast<int>(length) : INT_MAX);
}

}  // namespace detail
}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"

#endif  // ASIO_SSL_DETAIL_IMPL_ENGINE_OPENSSL_IPP
