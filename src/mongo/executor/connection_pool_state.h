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

#pragma once

namespace mongo {
namespace MONGO_MOD_PUBLIC executor {

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
     * The pool is expired and can be shutdown by updateController.
     * It is set when there have been no connection requests or in use connections for
     * ControllerInterface::hostTimeout().
     *
     * The host health will go back to `kHealthy` as soon as a connection is requested.
     */
    kExpired,
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
    /**
     * The pool has received an overload failure during a connection setup.
     * New connection spawns will happen with a backoff-with-jitter delay until the next
     * returned connection gets refreshed or a setup succeeds.
     */
    kThrottle,
};

}  // namespace MONGO_MOD_PUBLIC executor
}  // namespace mongo

