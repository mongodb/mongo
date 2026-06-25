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
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

inline constexpr std::string_view kInsertOnExistingDocMsg{"attempted to insert on existing doc"};
inline constexpr std::string_view kUpdateOnMissingDocMsg{
    "ran update as upsert and failed to match any documents"};
inline constexpr std::string_view kDeleteWasEmptyMsg{
    "applied a delete that did not delete anything"};
inline constexpr std::string_view kDeleteOnMissingNs{"applied a delete on missing namespace"};
inline constexpr std::string_view kAcceptableErrorInCommand{
    "received an acceptable error during oplog application"};
inline constexpr std::string_view kRecordIdsReplicatedDocIdMismatch{
    "the _id in the oplog entry for a replicated record id collection did not match the _id of the "
    "document found at the rid"};
inline constexpr std::string_view kReplicatedSizeDeltaMismatch{
    "replicated size delta mismatch between primary and secondary"};

enum class OplogConstraintViolationEnum {
    kInsertOnExistingDoc = 0,
    kUpdateOnMissingDoc,
    kDeleteWasEmpty,
    kDeleteOnMissingNs,
    kAcceptableErrorInCommand,
    kRecordIdsReplicatedDocIdMismatch,
    kReplicatedSizeDeltaMismatch,
    NUM_VIOLATION_TYPES,
};

// Returns a string describing the constraint violation of the given type.
std::string_view toString(OplogConstraintViolationEnum type);

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
    mutable std::mutex _mutex;

    std::vector<Date_t> _lastLogTimes = std::vector<Date_t>(
        static_cast<int>(OplogConstraintViolationEnum::NUM_VIOLATION_TYPES));  // (M)
};

}  // namespace repl
}  // namespace mongo
