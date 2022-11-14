/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"

namespace mongo {
/**
 * HedgeOptions contains the information necessary for a particular command invocation to execute as
 * a hedged read. Specifically, it contains:
 *      clang-format off
 *      (1) Whether or not the invocation should execute as a hedged read via `isHedgeEnabled`
 *      (2) How many hedged operations should be sent, in *addition* to the non-hedged/authoriative
 *          request (`hedgeCount`)
 *      (3) The maxTimeMS each hedge should be executed with (`maxTimeMSForHedgedReads`)
 *      clang-format on
 */
struct HedgeOptions {
    bool isHedgeEnabled = false;
    size_t hedgeCount = 0;
    int maxTimeMSForHedgedReads = 0;
};

/**
 * Return appropriate HedgeOptions for an invocation of the command named 'command' with
 * read preference 'readPref.' A command invocation can execute as a hedged read if all three of the
 * following conditions are met:
 *      clang-format off
 *      (1) The server has hedging globaly enabled
 *      (2) The read preference for the invocation allows for hedging
 *      (3) The command-type is read-only/is capable of hedging
 *      clang-format on
 */
HedgeOptions getHedgeOptions(StringData command, const ReadPreferenceSetting& readPref);

/**
 * We ignore a subset of errors that may occur while running hedged operations (e.g., maxTimeMS
 * expiration), as the operation may safely succeed despite their failure. For example, a network
 * timeout error indicates the remote host experienced a timeout while running a remote-command as
 * part of executing the hedged operation. This is by no means an indication that the operation has
 * failed, as other hedged operations may still succeed. This function returns 'true' if we should
 * ignore this error as a response to a hedged operation, and allow other hedges of the operation to
 * possibly succeed.
 * TODO SERVER-68704 will include other error categories that are safe to ignore.
 */
inline bool isIgnorableAsHedgeResult(const Status& status) {
    return status == ErrorCodes::MaxTimeMSExpired || status == ErrorCodes::StaleDbVersion ||
        ErrorCodes::isNetworkTimeoutError(status) || ErrorCodes::isStaleShardVersionError(status);
}
}  // namespace mongo
