// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
inline constexpr std::string_view kUpdateWasEmptyMsg{
    "applied an update that did not match any documents"};
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
    kUpdateWasEmpty,
    NUM_VIOLATION_TYPES,
};

// Returns a string describing the constraint violation of the given type.
std::string_view toString(OplogConstraintViolationEnum type);

/**
 * Logs oplog constraint violation occurrences.
 *
 * To avoid flooding the logs if continuous oplog constraint violations occur, we will only log
 * once every 10 minutes per each oplog constraint violation type.
 *
 * Before FCV 5.0 each node deleted excess capped documents independently, so capped collection
 * contents may have diverged between nodes, and later oplog entries on those collections can
 * harmlessly violate constraints. Violations that oplog application tolerates for this reason
 * ('cappedViolationIsTolerated') are logged separately from other violations, with their own log
 * id and rate limit, so that they cannot suppress the logging of real ones. Violations on capped
 * collections that oplog application does not tolerate are logged like any other violation.
 */
class OplogConstraintViolationLogger {
public:
    // Minimum period of time before logging another warning log message, set to 10min.
    static const Seconds kPeriodicLogTimeout;

    void logViolationIfReady(OplogConstraintViolationEnum type,
                             const BSONObj& obj,
                             boost::optional<Status> status,
                             bool isCappedCollection = false,
                             bool cappedViolationIsTolerated = false);

    // Clears the last log times so that every violation type can be logged again immediately.
    // Only for use in tests.
    void resetForTest();

private:
    mutable std::mutex _mutex;

    std::vector<Date_t> _lastLogTimes = std::vector<Date_t>(
        static_cast<int>(OplogConstraintViolationEnum::NUM_VIOLATION_TYPES));  // (M)
    std::vector<Date_t> _lastCappedLogTimes = std::vector<Date_t>(
        static_cast<int>(OplogConstraintViolationEnum::NUM_VIOLATION_TYPES));  // (M)
};

}  // namespace repl
}  // namespace mongo
