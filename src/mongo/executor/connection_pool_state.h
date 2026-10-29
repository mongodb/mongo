// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] executor {

/**
 * The set of states that a host pool can assume within the ConnectionPool class.
 * The comments below describe how this enum is used in said class.
 */
enum class ConnectionPoolState {
    /**
     * The pool is healthy and will spawn new connections to reach the specified target.
     */
    kHealthy,
    /**
     * The pool has processed a failure and will not spawn new connections until requested.
     * It is set by processFailure() unless a shutdown has been triggered.
     *
     * As a further note, this prevents us from spamming a failed host with connection attempts.
     * If an external user believes a host should be available, they can request again.
     *
     * The host health will go back to `kHealthy` as soon as a connection is requested.
     */
    kFailed,
    /**
     * The pool is shutdown and will never be called by the ConnectionPool again.
     * It is set by triggerShutdown() or updateController(). It is never unset.
     */
    kShutdown,
};

}  // namespace executor
}  // namespace mongo

