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
#include "mongo/executor/remote_command_request.h"

namespace mongo {

/**
 * Configure the hedge-related data members of `options`. If the command
 * specified by `cmdObj` and `readPref` is eligible for hedging, and hedging
 * is globally enabled for this server, then the hedge-related parameters of
 * `options` are set. Otherwise they are reset to their default values, which
 * specify unhedged behavior.
 */
void extractHedgeOptions(const BSONObj& cmdObj,
                         const ReadPreferenceSetting& readPref,
                         executor::RemoteCommandRequestOnAny::Options& options);

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
