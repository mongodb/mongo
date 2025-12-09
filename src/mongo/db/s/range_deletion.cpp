/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/range_deletion.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"

namespace mongo {

RangeDeletion::RangeDeletion(const RangeDeletionTask& task)
    : _taskId(task.getId()),
      _range(task.getRange()),
      _registrationTime(task.getTimestamp().value_or(
          Timestamp(getGlobalServiceContext()->getFastClockSource()->now()))) {}

RangeDeletion::~RangeDeletion() {
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(Status{ErrorCodes::Interrupted, "Range deletion interrupted"});
    }
}

const UUID& RangeDeletion::getTaskId() const {
    return _taskId;
}

const ChunkRange& RangeDeletion::getRange() const {
    return _range;
}

const Timestamp& RangeDeletion::getRegistrationTime() const {
    return _registrationTime;
}

SharedSemiFuture<void> RangeDeletion::getPendingFuture() {
    return _pendingPromise.getFuture();
}

void RangeDeletion::clearPending() {
    if (!_pendingPromise.getFuture().isReady()) {
        _pendingPromise.emplaceValue();
    }
}

SharedSemiFuture<void> RangeDeletion::getCompletionFuture() const {
    return _completionPromise.getFuture().semi().share();
}

void RangeDeletion::markComplete() {
    _completionPromise.emplaceValue();
}

}  // namespace mongo
