// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/net/ssl/context.hpp"
#include "mongo/util/net/ssl/error.hpp"

#include <cstring>

#include "asio/detail/config.hpp"
#include "asio/detail/throw_error.hpp"
#include "asio/error.hpp"

// This must be after all other includes
#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {


context::context(context::method m) : handle_(&_cred) {
    memset(&_cred, 0, sizeof(_cred));
    memset(&_tlsParams, 0, sizeof(_tlsParams));
    _cred.cTlsParameters = 1;
    _cred.pTlsParameters = &_tlsParams;
}

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
context::context(context&& other) {
    handle_ = other.handle_;
    other.handle_ = 0;
}

context& context::operator=(context&& other) {
    context tmp(ASIO_MOVE_CAST(context)(*this));
    handle_ = other.handle_;
    other.handle_ = 0;
    return *this;
}
#endif  // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

context::~context() {}

context::native_handle_type context::native_handle() {
    return handle_;
}

}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"
