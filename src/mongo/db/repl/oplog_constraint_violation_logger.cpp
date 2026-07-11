// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_constraint_violation_logger.h"

#include <string_view>

#include <boost/optional/optional.hpp>


// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/logv2/log.h"

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
        default:
            return "";
    }
}

void OplogConstraintViolationLogger::logViolationIfReady(OplogConstraintViolationEnum type,
                                                         const BSONObj& obj,
                                                         boost::optional<Status> status) {
    const auto index = static_cast<int>(type);

    std::lock_guard lk(_mutex);
    const auto lastLog = _lastLogTimes[index];
    const auto now = Date_t::now();

    if (now < lastLog + OplogConstraintViolationLogger::kPeriodicLogTimeout) {
        // We have logged this violation already within the last 10 minutes.
        return;
    }

    if (!status) {
        LOGV2_WARNING(7149000,
                      "Potential replication constraint violation during steady state replication",
                      "msg"_attr = toString(type),
                      "obj"_attr = obj);
    } else {
        LOGV2_WARNING(7149001,
                      "Potential replication constraint violation during steady state replication",
                      "msg"_attr = toString(type),
                      "obj"_attr = obj,
                      "status"_attr = *status);
    }

    // Update the last log time to now.
    _lastLogTimes[index] = now;
}

}  // namespace repl
}  // namespace mongo
