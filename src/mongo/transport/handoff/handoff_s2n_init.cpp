// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
