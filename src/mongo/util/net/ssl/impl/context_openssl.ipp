//
// ssl/impl/context.ipp
// ~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2005 Voipster / Indrek dot Juhani at voipster dot com
// Copyright (c) 2005-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_IMPL_CONTEXT_OPENSSL_IPP
#define ASIO_SSL_IMPL_CONTEXT_OPENSSL_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"
#include "mongo/util/net/ssl/context.hpp"
#include "mongo/util/net/ssl/error.hpp"
#include <cstring>

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

context::context(context::method m) : handle_(nullptr) {
    ::ERR_clear_error();

    switch (m) {
// SSL v2.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) || defined(OPENSSL_NO_SSL2)
        case context::sslv2:
        case context::sslv2_client:
        case context::sslv2_server:
            asio::detail::throw_error(asio::error::invalid_argument, "context");
            break;
#else   // (OPENSSL_VERSION_NUMBER >= 0x10100000L) || defined(OPENSSL_NO_SSL2)
        case context::sslv2:
            handle_ = ::SSL_CTX_new(::SSLv2_method());
            break;
        case context::sslv2_client:
            handle_ = ::SSL_CTX_new(::SSLv2_client_method());
            break;
        case context::sslv2_server:
            handle_ = ::SSL_CTX_new(::SSLv2_server_method());
            break;
#endif  // (OPENSSL_VERSION_NUMBER >= 0x10100000L) || defined(OPENSSL_NO_SSL2)

// SSL v3.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
        case context::sslv3:
            handle_ = ::SSL_CTX_new(::TLS_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, SSL3_VERSION);
                SSL_CTX_set_max_proto_version(handle_, SSL3_VERSION);
            }
            break;
        case context::sslv3_client:
            handle_ = ::SSL_CTX_new(::TLS_client_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, SSL3_VERSION);
                SSL_CTX_set_max_proto_version(handle_, SSL3_VERSION);
            }
            break;
        case context::sslv3_server:
            handle_ = ::SSL_CTX_new(::TLS_server_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, SSL3_VERSION);
                SSL_CTX_set_max_proto_version(handle_, SSL3_VERSION);
            }
            break;
#elif defined(OPENSSL_NO_SSL3)
        case context::sslv3:
        case context::sslv3_client:
        case context::sslv3_server:
            asio::detail::throw_error(asio::error::invalid_argument, "context");
            break;
#else   // defined(OPENSSL_NO_SSL3)
        case context::sslv3:
            handle_ = ::SSL_CTX_new(::SSLv3_method());
            break;
        case context::sslv3_client:
            handle_ = ::SSL_CTX_new(::SSLv3_client_method());
            break;
        case context::sslv3_server:
            handle_ = ::SSL_CTX_new(::SSLv3_server_method());
            break;
#endif  // defined(OPENSSL_NO_SSL3)

// TLS v1.0.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
        case context::tlsv1:
            handle_ = ::SSL_CTX_new(::TLS_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_VERSION);
            }
            break;
        case context::tlsv1_client:
            handle_ = ::SSL_CTX_new(::TLS_client_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_VERSION);
            }
            break;
        case context::tlsv1_server:
            handle_ = ::SSL_CTX_new(::TLS_server_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_VERSION);
            }
            break;
#else   // (OPENSSL_VERSION_NUMBER >= 0x10100000L)
        case context::tlsv1:
            handle_ = ::SSL_CTX_new(::TLSv1_method());
            break;
        case context::tlsv1_client:
            handle_ = ::SSL_CTX_new(::TLSv1_client_method());
            break;
        case context::tlsv1_server:
            handle_ = ::SSL_CTX_new(::TLSv1_server_method());
            break;
#endif  // (OPENSSL_VERSION_NUMBER >= 0x10100000L)

// TLS v1.1.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
        case context::tlsv11:
            handle_ = ::SSL_CTX_new(::TLS_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_1_VERSION);
            }
            break;
        case context::tlsv11_client:
            handle_ = ::SSL_CTX_new(::TLS_client_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_1_VERSION);
            }
            break;
        case context::tlsv11_server:
            handle_ = ::SSL_CTX_new(::TLS_server_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_1_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_1_VERSION);
            }
            break;
#elif defined(SSL_TXT_TLSV1_1)
        case context::tlsv11:
            handle_ = ::SSL_CTX_new(::TLSv1_1_method());
            break;
        case context::tlsv11_client:
            handle_ = ::SSL_CTX_new(::TLSv1_1_client_method());
            break;
        case context::tlsv11_server:
            handle_ = ::SSL_CTX_new(::TLSv1_1_server_method());
            break;
#else   // defined(SSL_TXT_TLSV1_1)
        case context::tlsv11:
        case context::tlsv11_client:
        case context::tlsv11_server:
            asio::detail::throw_error(asio::error::invalid_argument, "context");
            break;
#endif  // defined(SSL_TXT_TLSV1_1)

// TLS v1.2.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
        case context::tlsv12:
            handle_ = ::SSL_CTX_new(::TLS_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_2_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_2_VERSION);
            }
            break;
        case context::tlsv12_client:
            handle_ = ::SSL_CTX_new(::TLS_client_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_2_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_2_VERSION);
            }
            break;
        case context::tlsv12_server:
            handle_ = ::SSL_CTX_new(::TLS_server_method());
            if (handle_) {
                SSL_CTX_set_min_proto_version(handle_, TLS1_2_VERSION);
                SSL_CTX_set_max_proto_version(handle_, TLS1_2_VERSION);
            }
            break;
#elif defined(SSL_TXT_TLSV1_1)
        case context::tlsv12:
            handle_ = ::SSL_CTX_new(::TLSv1_2_method());
            break;
        case context::tlsv12_client:
            handle_ = ::SSL_CTX_new(::TLSv1_2_client_method());
            break;
        case context::tlsv12_server:
            handle_ = ::SSL_CTX_new(::TLSv1_2_server_method());
            break;
#else   // defined(SSL_TXT_TLSV1_1)
        case context::tlsv12:
        case context::tlsv12_client:
        case context::tlsv12_server:
            asio::detail::throw_error(asio::error::invalid_argument, "context");
            break;
#endif  // defined(SSL_TXT_TLSV1_1)

        // Any supported SSL/TLS version.
        case context::sslv23:
            handle_ = ::SSL_CTX_new(::SSLv23_method());
            break;
        case context::sslv23_client:
            handle_ = ::SSL_CTX_new(::SSLv23_client_method());
            break;
        case context::sslv23_server:
            handle_ = ::SSL_CTX_new(::SSLv23_server_method());
            break;

// Any supported TLS version.
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
        case context::tls:
            handle_ = ::SSL_CTX_new(::TLS_method());
            if (handle_)
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
            break;
        case context::tls_client:
            handle_ = ::SSL_CTX_new(::TLS_client_method());
            if (handle_)
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
            break;
        case context::tls_server:
            handle_ = ::SSL_CTX_new(::TLS_server_method());
            if (handle_)
                SSL_CTX_set_min_proto_version(handle_, TLS1_VERSION);
            break;
#else   // (OPENSSL_VERSION_NUMBER >= 0x10100000L)
        case context::tls:
            handle_ = ::SSL_CTX_new(::SSLv23_method());
            if (handle_)
                SSL_CTX_set_options(handle_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
            break;
        case context::tls_client:
            handle_ = ::SSL_CTX_new(::SSLv23_client_method());
            if (handle_)
                SSL_CTX_set_options(handle_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
            break;
        case context::tls_server:
            handle_ = ::SSL_CTX_new(::SSLv23_server_method());
            if (handle_)
                SSL_CTX_set_options(handle_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
            break;
#endif  // (OPENSSL_VERSION_NUMBER >= 0x10100000L)

        default:
            handle_ = ::SSL_CTX_new(nullptr);
            break;
    }

    if (handle_ == nullptr) {
        asio::error_code ec(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        asio::detail::throw_error(ec, "context");
    }
}

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
context::context(context&& other) {
    handle_ = other.handle_;
    other.handle_ = nullptr;
}

context& context::operator=(context&& other) {
    context tmp(ASIO_MOVE_CAST(context)(*this));
    handle_ = other.handle_;
    other.handle_ = nullptr;
    return *this;
}
#endif  // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

context::~context() {
    if (handle_) {
        if (SSL_CTX_get_app_data(handle_)) {
            SSL_CTX_set_app_data(handle_, nullptr);
        }

        ::SSL_CTX_free(handle_);
    }
}

context::native_handle_type context::native_handle() {
    return handle_;
}

}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"

#endif  // ASIO_SSL_IMPL_CONTEXT_OPENSSL_IPP
