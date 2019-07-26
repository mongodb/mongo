
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
#include "asio/io_context.hpp"
#include "mongo/util/net/ssl/context_base.hpp"
#include <string>

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

class context : public context_base, private noncopyable {
public:
    /// The native handle type of the SSL context.
    typedef SCHANNEL_CRED* native_handle_type;

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
    SCHANNEL_CRED _cred;

    // The underlying native implementation.
    native_handle_type handle_;
};

#include "asio/detail/pop_options.hpp"

#if defined(ASIO_HEADER_ONLY)
#include "mongo/util/net/ssl/impl/context_schannel.ipp"
#endif  // defined(ASIO_HEADER_ONLY)

}  // namespace ssl
}  // namespace asio
