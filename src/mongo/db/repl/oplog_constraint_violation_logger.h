/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

static constexpr StringData kInsertOnExistingDocMsg = "attempted to insert on existing doc"_sd;
static constexpr StringData kUpdateOnMissingDocMsg =
    "ran update as upsert and failed to match any documents"_sd;
static constexpr StringData kDeleteWasEmptyMsg = "applied a delete that did not delete anything"_sd;
static constexpr StringData kDeleteOnMissingNs = "applied a delete on missing namespace"_sd;
static constexpr StringData kAcceptableErrorInCommand =
    "received an acceptable error during oplog application"_sd;

enum class OplogConstraintViolationEnum {
    kInsertOnExistingDoc = 0,
    kUpdateOnMissingDoc,
    kDeleteWasEmpty,
    kDeleteOnMissingNs,
    kAcceptableErrorInCommand,
    NUM_VIOLATION_TYPES,
};

// Returns a string describing the constraint violation of the given type.
StringData toString(OplogConstraintViolationEnum type);

/**
 * Logs oplog constraint violation occurrences.
 *
 * To avoid flooding the logs if continuous oplog constraint violations occur, we will only log
 * once every 10 minutes per each oplog constraint violation type.
 */
class OplogConstraintViolationLogger {
public:
    // Minimum period of time before logging another warning log message, set to 10min.
    static const Seconds kPeriodicLogTimeout;

    void logViolationIfReady(OplogConstraintViolationEnum type,
                             const BSONObj& obj,
                             boost::optional<Status> status);

private:
    mutable stdx::mutex _mutex;

    std::vector<Date_t> _lastLogTimes = std::vector<Date_t>(
        static_cast<int>(OplogConstraintViolationEnum::NUM_VIOLATION_TYPES));  // (M)
};

}  // namespace repl
}  // namespace mongo
