/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationRollback


namespace mongo {
namespace repl {

// After the release of MongoDB 3.8, these fail point declarations can
// be moved into the rs_rollback.cpp file, as we no longer need to maintain
// functionality for rs_rollback_no_uuid.cpp. See SERVER-29766.

// Failpoint which causes rollback to hang before finishing.
MONGO_FAIL_POINT_DEFINE(rollbackHangBeforeFinish);

// Failpoint which causes rollback to hang and then fail after minValid is written.
MONGO_FAIL_POINT_DEFINE(rollbackHangThenFailAfterWritingMinValid);


namespace {

constexpr int kMaxConnectionAttempts = 3;

OpTime getOpTime(const OplogInterface::Iterator::Value& oplogValue) {
    return fassert(40298, OpTime::parseFromOplogEntry(oplogValue.first));
}

long long getTerm(const BSONObj& operation) {
    return operation["t"].numberLong();
}

Timestamp getTimestamp(const BSONObj& operation) {
    return operation["ts"].timestamp();
}

Timestamp getTimestamp(const OplogInterface::Iterator::Value& oplogValue) {
    return getTimestamp(oplogValue.first);
}

long long getTerm(const OplogInterface::Iterator::Value& oplogValue) {
    return getTerm(oplogValue.first);
}
}  // namespace

RollBackLocalOperations::RollBackLocalOperations(const OplogInterface& localOplog,
                                                 const RollbackOperationFn& rollbackOperation)

    : _localOplogIterator(localOplog.makeIterator()),
      _rollbackOperation(rollbackOperation),
      _scanned(0) {
    uassert(ErrorCodes::BadValue, "invalid local oplog iterator", _localOplogIterator);
    uassert(ErrorCodes::BadValue, "null roll back operation function", rollbackOperation);
}

RollBackLocalOperations::RollbackCommonPoint::RollbackCommonPoint(BSONObj oplogBSON,
                                                                  RecordId recordId,
                                                                  BSONObj nextOplogBSON)
    : _recordId(std::move(recordId)) {
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
    _opTime = oplogEntry.getOpTime();
    _wallClockTime = oplogEntry.getWallClockTime();
    // nextOplogEntry holds the oplog entry immediately after the common point.
    auto nextOplogEntry = uassertStatusOK(repl::OplogEntry::parse(nextOplogBSON));
    _firstWallClockTimeAfterCommonPoint = nextOplogEntry.getWallClockTime();
}

StatusWith<RollBackLocalOperations::RollbackCommonPoint> RollBackLocalOperations::onRemoteOperation(
    const BSONObj& operation) {
    if (_scanned == 0) {
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            return Status(ErrorCodes::OplogStartMissing, "no oplog during rollback");
        }
        _localOplogValue = result.getValue();
    }

    // As we iterate through the oplog in reverse, opAfterCurrentEntry holds the oplog entry
    // immediately after the entry stored in _localOplogValue.
    BSONObj opAfterCurrentEntry = _localOplogValue.first;

    while (getTimestamp(_localOplogValue) > getTimestamp(operation)) {
        _scanned++;
        LOGV2_DEBUG(21656,
                    2,
                    "Local oplog entry to roll back: {oplogEntry}",
                    "Local oplog entry to roll back",
                    "oplogEntry"_attr = redact(_localOplogValue.first));
        auto status = _rollbackOperation(_localOplogValue.first);
        if (!status.isOK()) {
            invariant(ErrorCodes::NoSuchKey != status.code());
            return status;
        }
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            return Status(ErrorCodes::NoMatchingDocument,
                          str::stream()
                              << "reached beginning of local oplog: {"
                              << "scanned: " << _scanned
                              << ", theirTime: " << getTimestamp(operation).toString()
                              << ", ourTime: " << getTimestamp(_localOplogValue).toString() << "}");
        }
        opAfterCurrentEntry = _localOplogValue.first;
        _localOplogValue = result.getValue();
    }

    if (getTimestamp(_localOplogValue) == getTimestamp(operation)) {
        _scanned++;
        if (getTerm(_localOplogValue) == getTerm(operation)) {
            return RollbackCommonPoint(
                _localOplogValue.first, _localOplogValue.second, opAfterCurrentEntry);
        }

        // We don't need to advance the localOplogIterator here because it is guaranteed to advance
        // during the next call to onRemoteOperation. This is because before the next call to
        // onRemoteOperation, the remote oplog iterator will advance and the new remote operation is
        // guaranteed to have a timestamp less than the current local operation, which will trigger
        // a call to get the next local operation.
        return Status(ErrorCodes::NoSuchKey,
                      "Unable to determine common point - same timestamp but different terms. "
                      "Need to process additional remote operations.");
    }

    invariant(getTimestamp(_localOplogValue) < getTimestamp(operation));
    _scanned++;
    return Status(ErrorCodes::NoSuchKey,
                  "Unable to determine common point. "
                  "Need to process additional remote operations.");
}

StatusWith<RollBackLocalOperations::RollbackCommonPoint> syncRollBackLocalOperations(
    const OplogInterface& localOplog,
    const OplogInterface& remoteOplog,
    const RollBackLocalOperations::RollbackOperationFn& rollbackOperation) {

    std::unique_ptr<OplogInterface::Iterator> remoteIterator;

    // Retry in case of network errors.
    for (int attemptsLeft = kMaxConnectionAttempts - 1; attemptsLeft >= 0; attemptsLeft--) {
        try {
            remoteIterator = remoteOplog.makeIterator();
        } catch (DBException&) {
            if (attemptsLeft == 0) {
                throw;
            }
        }
    }

    invariant(remoteIterator);
    auto remoteResult = remoteIterator->next();
    if (!remoteResult.isOK()) {
        return Status(ErrorCodes::InvalidSyncSource, remoteResult.getStatus().reason())
            .withContext("remote oplog empty or unreadable");
    }

    RollBackLocalOperations finder(localOplog, rollbackOperation);
    Timestamp theirTime;
    while (remoteResult.isOK()) {
        BSONObj theirObj = remoteResult.getValue().first;
        theirTime = theirObj["ts"].timestamp();
        auto result = finder.onRemoteOperation(theirObj);
        if (result.isOK()) {
            return result.getValue();
        } else if (result.getStatus().code() != ErrorCodes::NoSuchKey) {
            return result;
        }
        remoteResult = remoteIterator->next();
    }
    return Status(ErrorCodes::NoMatchingDocument,
                  str::stream() << "reached beginning of remote oplog: {"
                                << "them: " << remoteOplog.toString()
                                << ", theirTime: " << theirTime.toString() << "}");
}

}  // namespace repl
}  // namespace mongo
