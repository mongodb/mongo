//
// ssl/context.hpp
// ~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_CONTEXT_OPENSSL_HPP
#define ASIO_SSL_CONTEXT_OPENSSL_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif  // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#include "asio/buffer.hpp"
#include "asio/io_context.hpp"
#include "mongo/util/net/ssl/context_base.hpp"
#include "mongo/util/net/ssl/detail/openssl_types.hpp"
#include <string>

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

class context : public context_base, private noncopyable {
public:
    /// The native handle type of the SSL context.
    typedef SSL_CTX* native_handle_type;

    /// Constructor.
    ASIO_DECL explicit context(method m);

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
    /// Move-construct a context from another.
    /**
     * This constructor moves an SSL context from one object to another.
     *
     * @param other The other context object from which the move will occur.
     *
     * @note Following the move, the following operations only are valid for the
     * moved-from object:
     * @li Destruction.
     * @li As a target for move-assignment.
     */
    ASIO_DECL context(context&& other);

    /// Move-assign a context from another.
    /**
     * This assignment operator moves an SSL context from one object to another.
     *
     * @param other The other context object from which the move will occur.
     *
     * @note Following the move, the following operations only are valid for the
     * moved-from object:
     * @li Destruction.
     * @li As a target for move-assignment.
     */
    ASIO_DECL context& operator=(context&& other);
#endif  // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

    /// Destructor.
    ASIO_DECL ~context();

    /// Get the underlying implementation in the native type.
    /**
     * This function may be used to obtain the underlying implementation of the
     * context. This is intended to allow access to context functionality that is
     * not otherwise provided.
     */
    ASIO_DECL native_handle_type native_handle();

private:
    // The underlying native implementation.
    native_handle_type handle_;
};

}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
#include "mongo/util/net/ssl/impl/context_openssl.ipp"
#endif  // defined(ASIO_HEADER_ONLY)

#endif  // ASIO_SSL_CONTEXT_OPENSSL_HPP
