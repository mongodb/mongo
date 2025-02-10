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

#include "mongo/db/repl/oplog_constraint_violation_logger.h"

#include <boost/optional/optional.hpp>


// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

// Default interval set to 10 minutes.
const Seconds OplogConstraintViolationLogger::kPeriodicLogTimeout(60 * 10);

StringData toString(OplogConstraintViolationEnum type) {
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
        default:
            return "";
    }
}

void OplogConstraintViolationLogger::logViolationIfReady(OplogConstraintViolationEnum type,
                                                         const BSONObj& obj,
                                                         boost::optional<Status> status) {
    const auto index = static_cast<int>(type);

    stdx::lock_guard lk(_mutex);
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
