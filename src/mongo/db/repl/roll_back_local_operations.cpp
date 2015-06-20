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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/roll_back_local_operations.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

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

StatusWith<RollBackLocalOperations::RollbackCommonPoint> RollBackLocalOperations::onRemoteOperation(
    const BSONObj& operation) {
    if (_scanned == 0) {
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            return StatusWith<RollbackCommonPoint>(ErrorCodes::OplogStartMissing,
                                                   "no oplog during initsync");
        }
        _localOplogValue = result.getValue();

        long long diff = static_cast<long long>(getTimestamp(_localOplogValue).getSecs()) -
            getTimestamp(operation).getSecs();
        // diff could be positive, negative, or zero
        log() << "rollback our last optime:   " << getTimestamp(_localOplogValue).toStringPretty();
        log() << "rollback their last optime: " << getTimestamp(operation).toStringPretty();
        log() << "rollback diff in end of log times: " << diff << " seconds";
        if (diff > 1800) {
            severe() << "rollback too long a time period for a rollback.";
            return StatusWith<RollbackCommonPoint>(
                ErrorCodes::ExceededTimeLimit,
                "rollback error: not willing to roll back more than 30 minutes of data");
        }
    }

    while (getTimestamp(_localOplogValue) > getTimestamp(operation)) {
        _scanned++;
        auto status = _rollbackOperation(_localOplogValue.first);
        if (!status.isOK()) {
            invariant(ErrorCodes::NoSuchKey != status.code());
            return status;
        }
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            severe() << "rollback error RS101 reached beginning of local oplog";
            log() << "    scanned: " << _scanned;
            log() << "  theirTime: " << getTimestamp(operation).toStringLong();
            log() << "  ourTime:   " << getTimestamp(_localOplogValue).toStringLong();
            return StatusWith<RollbackCommonPoint>(ErrorCodes::NoMatchingDocument,
                                                   "RS101 reached beginning of local oplog [2]");
        }
        _localOplogValue = result.getValue();
    }

    if (getTimestamp(_localOplogValue) == getTimestamp(operation)) {
        _scanned++;
        if (getHash(_localOplogValue) == getHash(operation)) {
            return StatusWith<RollbackCommonPoint>(
                std::make_pair(getTimestamp(_localOplogValue), _localOplogValue.second));
        }
        auto status = _rollbackOperation(_localOplogValue.first);
        if (!status.isOK()) {
            invariant(ErrorCodes::NoSuchKey != status.code());
            return status;
        }
        auto result = _localOplogIterator->next();
        if (!result.isOK()) {
            severe() << "rollback error RS101 reached beginning of local oplog";
            log() << "    scanned: " << _scanned;
            log() << "  theirTime: " << getTimestamp(operation).toStringLong();
            log() << "  ourTime:   " << getTimestamp(_localOplogValue).toStringLong();
            return StatusWith<RollbackCommonPoint>(ErrorCodes::NoMatchingDocument,
                                                   "RS101 reached beginning of local oplog [1]");
        }
        _localOplogValue = result.getValue();
        return StatusWith<RollbackCommonPoint>(
            ErrorCodes::NoSuchKey,
            "Unable to determine common point - same timestamp but different hash. "
            "Need to process additional remote operations.");
    }

    if (getTimestamp(_localOplogValue) < getTimestamp(operation)) {
        _scanned++;
        return StatusWith<RollbackCommonPoint>(ErrorCodes::NoSuchKey,
                                               "Unable to determine common point. "
                                               "Need to process additional remote operations.");
    }

    return RollbackCommonPoint(Timestamp(Seconds(1), 0), RecordId());
}

StatusWith<RollBackLocalOperations::RollbackCommonPoint> syncRollBackLocalOperations(
    const OplogInterface& localOplog,
    const OplogInterface& remoteOplog,
    const RollBackLocalOperations::RollbackOperationFn& rollbackOperation) {
    auto remoteIterator = remoteOplog.makeIterator();
    auto remoteResult = remoteIterator->next();
    if (!remoteResult.isOK()) {
        return StatusWith<RollBackLocalOperations::RollbackCommonPoint>(
            ErrorCodes::InvalidSyncSource, "remote oplog empty or unreadable");
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

    severe() << "rollback error RS100 reached beginning of remote oplog";
    log() << "  them:      " << remoteOplog.toString();
    log() << "  theirTime: " << theirTime.toStringLong();
    return StatusWith<RollBackLocalOperations::RollbackCommonPoint>(
        ErrorCodes::NoMatchingDocument, "RS100 reached beginning of remote oplog [1]");
}

}  // namespace repl
}  // namespace mongo
