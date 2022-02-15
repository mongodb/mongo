/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/db/concurrency/temporarily_unavailable_exception.h"
#include "mongo/base/string_data.h"
#include "mongo/db/server_options_general_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"

namespace mongo {

TemporarilyUnavailableException::TemporarilyUnavailableException(StringData context)
    : DBException(Status(ErrorCodes::TemporarilyUnavailable, context)) {
    invariant(gLoadShedding);
}

void TemporarilyUnavailableException::handle(OperationContext* opCtx,
                                             int attempts,
                                             StringData opStr,
                                             StringData ns,
                                             const TemporarilyUnavailableException& e) {
    opCtx->recoveryUnit()->abandonSnapshot();
    if (opCtx->getClient()->isFromUserConnection() &&
        attempts > TemporarilyUnavailableException::kMaxRetryAttempts) {
        LOGV2_DEBUG(6083901,
                    1,
                    "Too many TemporarilyUnavailableException's, giving up",
                    "attempts"_attr = attempts,
                    "operation"_attr = opStr,
                    logAttrs(NamespaceString(ns)));
        throw e;
    }
    LOGV2_DEBUG(6083900,
                1,
                "Caught TemporarilyUnavailableException",
                "attempts"_attr = attempts,
                "operation"_attr = opStr,
                logAttrs(NamespaceString(ns)));
    // Back off linearly with the retry attempt number.
    opCtx->sleepFor(TemporarilyUnavailableException::kRetryBackoff * attempts);
}

}  // namespace mongo
