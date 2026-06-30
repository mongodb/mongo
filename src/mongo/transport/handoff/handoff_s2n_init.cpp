/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/transport/handoff/handoff_s2n_init.h"

#include "mongo/util/static_immortal.h"

#include <s2n.h>

#include <fmt/format.h>

namespace mongo {
namespace {

/**
 * s2n-tls's documentation states that `s2n_init()` must be called before any of the library's
 * facilities are used.
 * `s2n_init`'s contract states that it must not be called more than once.
 * `s2n_cleanup` is a deprecated no-op, so when `s2n_init` says not more than once, it means process
 * wide.
 * s2n-tls does not provide a public "is initialized" function.
 * As an alternative, we use a trick learned from aws-sdk-cpp's `s2n_tls_channel_handler.c`.
 * `s2n_disable_atexit()`, which is harmless because s2n-tls's atexit support is disabled by
 * default, will fail if `s2n_init` has been called previously. So, we can use `s2n_disable_atexit`
 * to check whether the library has been initialized.
 * This technically violates the "`s2n_init()` must be called before any of the library's
 * facilities" contract, but works.
 */
struct S2NInitOnce {
    S2NInitOnce() : status(Status::OK()) {
        if (s2n_disable_atexit() != S2N_SUCCESS) {
            return;  // already initialized
        }
        if (s2n_init() != S2N_SUCCESS) {
            status = Status(ErrorCodes::InternalError,
                            fmt::format("s2n_init failed: {}", s2n_strerror(s2n_errno, "EN")));
        }
    }

    Status status;
};

}  // namespace

Status s2nInitOnce() {
    static const StaticImmortal<S2NInitOnce> instance;
    return instance->status;
}

}  // namespace mongo
