/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationRollback

#include "mongo/platform/basic.h"

#include "mongo/db/repl/roll_back_local_operations.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

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

Timestamp getTimestamp(const BSONObj& operation) {
    return operation["ts"].timestamp();
}

Timestamp getTimestamp(const OplogInterface::Iterator::Value& oplogValue) {
    return getTimestamp(oplogValue.first);
}

long long getHash(const BSONObj& operation) {
    return operation["h"].Long();
}

long long getHash(const OplogInterface::Iterator::Value& oplogValue) {
    return getHash(oplogValue.first);
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
                                                                  RecordId recordId)
    : _recordId(std::move(recordId)) {
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
    _opTime = oplogEntry.getOpTime();
    _wallClockTime = oplogEntry.getWallClockTime();
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

    while (getTimestamp(_localOplogValue) > getTimestamp(operation)) {
        _scanned++;
        LOG(2) << "Local oplog entry to roll back: " << redact(_localOplogValue.first);
        auto status = _rollbackOperation(_localOplogValue.first);
        if (!status.isOK()) {
            invariant(ErrorCodes::NoSuchKey != status.code());
            return status;
        }
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            return Status(ErrorCodes::NoMatchingDocument,
                          str::stream() << "reached beginning of local oplog: {"
                                        << "scanned: "
                                        << _scanned
                                        << ", theirTime: "
                                        << getTimestamp(operation).toString()
                                        << ", ourTime: "
                                        << getTimestamp(_localOplogValue).toString()
                                        << "}");
        }
        _localOplogValue = result.getValue();
    }

    if (getTimestamp(_localOplogValue) == getTimestamp(operation)) {
        _scanned++;
        if (getHash(_localOplogValue) == getHash(operation)) {
            return RollbackCommonPoint(_localOplogValue.first, _localOplogValue.second);
        }

        LOG(2) << "Local oplog entry to roll back: " << redact(_localOplogValue.first);
        auto status = _rollbackOperation(_localOplogValue.first);
        if (!status.isOK()) {
            invariant(ErrorCodes::NoSuchKey != status.code());
            return status;
        }
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            return Status(ErrorCodes::NoMatchingDocument,
                          str::stream() << "reached beginning of local oplog: {"
                                        << "scanned: "
                                        << _scanned
                                        << ", theirTime: "
                                        << getTimestamp(operation).toString()
                                        << ", ourTime: "
                                        << getTimestamp(_localOplogValue).toString()
                                        << "}");
        }
        _localOplogValue = result.getValue();
        return Status(ErrorCodes::NoSuchKey,
                      "Unable to determine common point - same timestamp but different hash. "
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
        theirTime = remoteResult.getValue().first["ts"].timestamp();
        BSONObj theirObj = remoteResult.getValue().first;
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
                                << "them: "
                                << remoteOplog.toString()
                                << ", theirTime: "
                                << theirTime.toString()
                                << "}");
}

}  // namespace repl
}  // namespace mongo
