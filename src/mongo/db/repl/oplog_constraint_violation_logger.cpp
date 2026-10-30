// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_constraint_violation_logger.h"

#include "mongo/logv2/log.h"

#include <algorithm>
#include <string_view>

#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

// Default interval set to 10 minutes.
const Seconds OplogConstraintViolationLogger::kPeriodicLogTimeout(60 * 10);

std::string_view toString(OplogConstraintViolationEnum type) {
    switch (type) {
        case OplogConstraintViolationEnum::kInsertOnExistingDoc:
            return kInsertOnExistingDocMsg;
        case OplogConstraintViolationEnum::kUpdateOnMissingDoc:
            return kUpdateOnMissingDocMsg;
        case OplogConstraintViolationEnum::kDeleteWasEmpty:
            return kDeleteWasEmptyMsg;
        case OplogConstraintViolationEnum::kDeleteOnMissingNs:
            return kDeleteOnMissingNs;
        case OplogConstraintViolationEnum::kAcceptableErrorInCommand:
            return kAcceptableErrorInCommand;
        case mongo::repl::OplogConstraintViolationEnum::kRecordIdsReplicatedDocIdMismatch:
            return kRecordIdsReplicatedDocIdMismatch;
        case mongo::repl::OplogConstraintViolationEnum::kReplicatedSizeDeltaMismatch:
            return kReplicatedSizeDeltaMismatch;
        case OplogConstraintViolationEnum::kUpdateWasEmpty:
            return kUpdateWasEmptyMsg;
        default:
            return "";
    }
}

void OplogConstraintViolationLogger::logViolationIfReady(OplogConstraintViolationEnum type,
                                                         const BSONObj& obj,
                                                         boost::optional<Status> status,
                                                         bool isCappedCollection,
                                                         bool cappedViolationIsTolerated) {
    tassert(10836102,
            "Only violations on capped collections can be tolerated",
            isCappedCollection || !cappedViolationIsTolerated);
    const auto index = static_cast<int>(type);

    std::lock_guard lk(_mutex);
    auto& lastLogTimes = cappedViolationIsTolerated ? _lastCappedLogTimes : _lastLogTimes;
    const auto lastLog = lastLogTimes[index];
    const auto now = Date_t::now();

    if (now < lastLog + OplogConstraintViolationLogger::kPeriodicLogTimeout) {
        // We have logged this violation already within the last 10 minutes.
        return;
    }

    if (cappedViolationIsTolerated) {
        // Capped collections may have diverged between nodes before FCV 5.0, and oplog application
        // tolerates this violation, so it is expected and harmless. Log it separately from other
        // constraint violations.
        if (!status) {
            LOGV2(10836100,
                  "Replication constraint violation on a capped collection during steady state "
                  "replication. This is expected if capped collection contents diverged between "
                  "nodes and is harmless",
                  "msg"_attr = toString(type),
                  "isCappedCollection"_attr = isCappedCollection,
                  "obj"_attr = obj);
        } else {
            LOGV2(10836101,
                  "Replication constraint violation on a capped collection during steady state "
                  "replication. This is expected if capped collection contents diverged between "
                  "nodes and is harmless",
                  "msg"_attr = toString(type),
                  "isCappedCollection"_attr = isCappedCollection,
                  "obj"_attr = obj,
                  "status"_attr = *status);
        }
    } else if (!status) {
        LOGV2_WARNING(7149000,
                      "Potential replication constraint violation during steady state replication",
                      "msg"_attr = toString(type),
                      "isCappedCollection"_attr = isCappedCollection,
                      "obj"_attr = obj);
    } else {
        LOGV2_WARNING(7149001,
                      "Potential replication constraint violation during steady state replication",
                      "msg"_attr = toString(type),
                      "isCappedCollection"_attr = isCappedCollection,
                      "obj"_attr = obj,
                      "status"_attr = *status);
    }

    // Update the last log time to now.
    lastLogTimes[index] = now;
}

void OplogConstraintViolationLogger::resetForTest() {
    std::lock_guard lk(_mutex);
    std::fill(_lastLogTimes.begin(), _lastLogTimes.end(), Date_t());
    std::fill(_lastCappedLogTimes.begin(), _lastCappedLogTimes.end(), Date_t());
}

}  // namespace repl
}  // namespace mongo
