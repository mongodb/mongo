
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
#include "asio/detail/noncopyable.hpp"
#include "mongo/util/net/ssl/apple.hpp"
#include "mongo/util/net/ssl/context_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

class context : public context_base, private noncopyable {
public:
    using native_handle_type = apple::Context*;

    ASIO_DECL explicit context(method m) {
        _context.protoMin = _mapProto(m);
        _context.protoMax = _context.protoMax;
    }

    ASIO_DECL context(context&& other) = default;
    ASIO_DECL context& operator=(context&& other) = default;

    ASIO_DECL native_handle_type native_handle() {
        return &_context;
    }

private:
    static ::SSLProtocol _mapProto(method m) {
        switch (m) {
            case context::tlsv1:
            case context::tlsv1_client:
            case context::tlsv1_server:
                return ::kTLSProtocol1;
            case context::tlsv11:
            case context::tlsv11_client:
            case context::tlsv11_server:
                return ::kTLSProtocol11;
            case context::tlsv12:
            case context::tlsv12_client:
            case context::tlsv12_server:
            default:
                return ::kTLSProtocol12;
        }
    }

    apple::Context _context;
};

}  // namespace ssl
}  // namespace asio

#include "asio/detail/pop_options.hpp"
