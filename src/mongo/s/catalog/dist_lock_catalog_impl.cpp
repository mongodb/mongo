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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/dist_lock_catalog_impl.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/wc_error_detail.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

namespace {

const char kCmdResponseWriteConcernField[] = "writeConcernError";
const char kFindAndModifyResponseResultDocField[] = "value";
const char kLocalTimeField[] = "localTime";
const ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly, TagSet());

/**
 * Returns the resulting new object from the findAndModify response object.
 * Returns LockStateChangeFailed if value field was null, which indicates that
 * the findAndModify command did not modify any document.
 * This also checks for errors in the response object.
 */
StatusWith<BSONObj> extractFindAndModifyNewObj(const BSONObj& responseObj) {
    auto cmdStatus = getStatusFromCommandResult(responseObj);

    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    BSONElement wcErrorElem;
    auto wcErrStatus =
        bsonExtractTypedField(responseObj, kCmdResponseWriteConcernField, Object, &wcErrorElem);

    if (wcErrStatus.isOK()) {
        BSONObj wcErrObj(wcErrorElem.Obj());
        WCErrorDetail wcError;

        string wcErrorParseMsg;
        if (!wcError.parseBSON(wcErrObj, &wcErrorParseMsg)) {
            return Status(ErrorCodes::UnsupportedFormat, wcErrorParseMsg);
        }

        return {ErrorCodes::WriteConcernFailed, wcError.getErrMessage()};
    }

    if (wcErrStatus != ErrorCodes::NoSuchKey) {
        return wcErrStatus;
    }

    if (const auto& newDocElem = responseObj[kFindAndModifyResponseResultDocField]) {
        if (newDocElem.isNull()) {
            return {ErrorCodes::LockStateChangeFailed,
                    "findAndModify query predicate didn't match any lock document"};
        }

        if (!newDocElem.isABSONObj()) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "expected an object from the findAndModify response '"
                                  << kFindAndModifyResponseResultDocField
                                  << "'field, got: " << newDocElem};
        }

        return newDocElem.Obj();
    }

    return {ErrorCodes::UnsupportedFormat,
            str::stream() << "no '" << kFindAndModifyResponseResultDocField
                          << "' in findAndModify response"};
}

/**
 * Extract the electionId from a command response.
 *
 * TODO: this needs to support OP_COMMAND metadata.
 */
StatusWith<OID> extractElectionId(const BSONObj& responseObj) {
    BSONElement gleStatsElem;
    auto gleStatus = bsonExtractTypedField(responseObj, "$gleStats", Object, &gleStatsElem);

    if (!gleStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, gleStatus.reason()};
    }

    OID electionId;

    auto electionIdStatus = bsonExtractOIDField(gleStatsElem.Obj(), "electionId", &electionId);

    if (!electionIdStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, electionIdStatus.reason()};
    }

    return electionId;
}

}  // unnamed namespace

DistLockCatalogImpl::DistLockCatalogImpl(ShardRegistry* shardRegistry,
                                         Milliseconds writeConcernTimeout)
    : _client(shardRegistry),
      _writeConcern(WriteConcernOptions(WriteConcernOptions::kMajority,
                                        // Note: Even though we're setting NONE here,
                                        // kMajority implies JOURNAL, if journaling is supported
                                        // by this mongod.
                                        WriteConcernOptions::NONE,
                                        writeConcernTimeout.count())),
      _lockPingNS(LockpingsType::ConfigNS),
      _locksNS(LocksType::ConfigNS) {}

DistLockCatalogImpl::~DistLockCatalogImpl() = default;

RemoteCommandTargeter* DistLockCatalogImpl::_targeter() {
    return _client->getShard("config")->getTargeter();
}

StatusWith<LockpingsType> DistLockCatalogImpl::getPing(StringData processID) {
    auto targetStatus = _targeter()->findHost(kReadPref);

    if (!targetStatus.isOK()) {
        return targetStatus.getStatus();
    }

    auto findResult = _client->exhaustiveFind(targetStatus.getValue(),
                                              _lockPingNS,
                                              BSON(LockpingsType::process() << processID),
                                              BSONObj(),
                                              1);

    if (!findResult.isOK()) {
        return findResult.getStatus();
    }

    const auto& findResultSet = findResult.getValue();

    if (findResultSet.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "ping entry for " << processID << " not found"};
    }

    BSONObj doc = findResultSet.front();
    auto pingDocResult = LockpingsType::fromBSON(doc);
    if (!pingDocResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "failed to parse document: " << doc << " : "
                              << pingDocResult.getStatus().toString()};
    }

    return pingDocResult.getValue();
}

Status DistLockCatalogImpl::ping(StringData processID, Date_t ping) {
    auto request =
        FindAndModifyRequest::makeUpdate(_lockPingNS,
                                         BSON(LockpingsType::process() << processID),
                                         BSON("$set" << BSON(LockpingsType::ping(ping))));
    request.setUpsert(true);
    request.setWriteConcern(_writeConcern);

    auto resultStatus = _client->runCommandWithNotMasterRetries(
        "config", _locksNS.db().toString(), request.toBSON());

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());
    auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
    return findAndModifyStatus.getStatus();
}

StatusWith<LocksType> DistLockCatalogImpl::grabLock(StringData lockID,
                                                    const OID& lockSessionID,
                                                    StringData who,
                                                    StringData processId,
                                                    Date_t time,
                                                    StringData why) {
    BSONObj newLockDetails(BSON(LocksType::lockID(lockSessionID)
                                << LocksType::state(LocksType::LOCKED) << LocksType::who() << who
                                << LocksType::process() << processId << LocksType::when(time)
                                << LocksType::why() << why));

    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::name() << lockID << LocksType::state(LocksType::UNLOCKED)),
        BSON("$set" << newLockDetails));
    request.setUpsert(true);
    request.setShouldReturnNew(true);
    request.setWriteConcern(_writeConcern);

    auto resultStatus = _client->runCommandWithNotMasterRetries(
        "config", _locksNS.db().toString(), request.toBSON());

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());

    auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
    if (!findAndModifyStatus.isOK()) {
        if (findAndModifyStatus == ErrorCodes::DuplicateKey) {
            // Another thread won the upsert race. Also see SERVER-14322.
            return {ErrorCodes::LockStateChangeFailed,
                    str::stream() << "duplicateKey error during upsert of lock: " << lockID};
        }

        return findAndModifyStatus.getStatus();
    }

    BSONObj doc = findAndModifyStatus.getValue();
    auto locksTypeResult = LocksType::fromBSON(doc);
    if (!locksTypeResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "failed to parse: " << doc << " : "
                              << locksTypeResult.getStatus().toString()};
    }

    return locksTypeResult.getValue();
}

StatusWith<LocksType> DistLockCatalogImpl::overtakeLock(StringData lockID,
                                                        const OID& lockSessionID,
                                                        const OID& currentHolderTS,
                                                        StringData who,
                                                        StringData processId,
                                                        Date_t time,
                                                        StringData why) {
    BSONArrayBuilder orQueryBuilder;
    orQueryBuilder.append(
        BSON(LocksType::name() << lockID << LocksType::state(LocksType::UNLOCKED)));
    orQueryBuilder.append(BSON(LocksType::name() << lockID << LocksType::lockID(currentHolderTS)));

    BSONObj newLockDetails(BSON(LocksType::lockID(lockSessionID)
                                << LocksType::state(LocksType::LOCKED) << LocksType::who() << who
                                << LocksType::process() << processId << LocksType::when(time)
                                << LocksType::why() << why));

    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS, BSON("$or" << orQueryBuilder.arr()), BSON("$set" << newLockDetails));
    request.setShouldReturnNew(true);
    request.setWriteConcern(_writeConcern);

    auto resultStatus = _client->runCommandWithNotMasterRetries(
        "config", _locksNS.db().toString(), request.toBSON());

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());

    auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
    if (!findAndModifyStatus.isOK()) {
        return findAndModifyStatus.getStatus();
    }

    BSONObj doc = findAndModifyStatus.getValue();
    auto locksTypeResult = LocksType::fromBSON(doc);
    if (!locksTypeResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "failed to parse: " << doc << " : "
                              << locksTypeResult.getStatus().toString()};
    }

    return locksTypeResult.getValue();
}

Status DistLockCatalogImpl::unlock(const OID& lockSessionID) {
    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::lockID(lockSessionID)),
        BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
    request.setWriteConcern(_writeConcern);

    auto resultStatus = _client->runCommandWithNotMasterRetries(
        "config", _locksNS.db().toString(), request.toBSON());

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());

    auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj).getStatus();

    if (findAndModifyStatus == ErrorCodes::LockStateChangeFailed) {
        // Did not modify any document, which implies that the lock already has a
        // a different owner. This is ok since it means that the objective of
        // releasing ownership of the lock has already been accomplished.
        return Status::OK();
    }

    return findAndModifyStatus;
}

StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogImpl::getServerInfo() {
    auto targetStatus = _targeter()->findHost(kReadPref);

    if (!targetStatus.isOK()) {
        return targetStatus.getStatus();
    }

    auto resultStatus =
        _client->runCommand(targetStatus.getValue(), "admin", BSON("serverStatus" << 1));

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());

    auto cmdStatus = getStatusFromCommandResult(responseObj);

    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    BSONElement localTimeElem;
    auto localTimeStatus =
        bsonExtractTypedField(responseObj, kLocalTimeField, Date, &localTimeElem);

    if (!localTimeStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, localTimeStatus.reason()};
    }

    auto electionIdStatus = extractElectionId(responseObj);

    if (!electionIdStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, electionIdStatus.getStatus().reason()};
    }

    return DistLockCatalog::ServerInfo(localTimeElem.date(), electionIdStatus.getValue());
}

StatusWith<LocksType> DistLockCatalogImpl::getLockByTS(const OID& lockSessionID) {
    auto targetStatus = _targeter()->findHost(kReadPref);

    if (!targetStatus.isOK()) {
        return targetStatus.getStatus();
    }

    auto findResult = _client->exhaustiveFind(
        targetStatus.getValue(), _locksNS, BSON(LocksType::lockID(lockSessionID)), BSONObj(), 1);

    if (!findResult.isOK()) {
        return findResult.getStatus();
    }

    const auto& findResultSet = findResult.getValue();

    if (findResultSet.empty()) {
        return {ErrorCodes::LockNotFound,
                str::stream() << "lock with ts " << lockSessionID << " not found"};
    }

    BSONObj doc = findResultSet.front();
    auto locksTypeResult = LocksType::fromBSON(doc);
    if (!locksTypeResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "failed to parse: " << doc << " : "
                              << locksTypeResult.getStatus().toString()};
    }

    return locksTypeResult.getValue();
}

StatusWith<LocksType> DistLockCatalogImpl::getLockByName(StringData name) {
    auto targetStatus = _targeter()->findHost(kReadPref);

    if (!targetStatus.isOK()) {
        return targetStatus.getStatus();
    }

    auto findResult = _client->exhaustiveFind(
        targetStatus.getValue(), _locksNS, BSON(LocksType::name() << name), BSONObj(), 1);

    if (!findResult.isOK()) {
        return findResult.getStatus();
    }

    const auto& findResultSet = findResult.getValue();

    if (findResultSet.empty()) {
        return {ErrorCodes::LockNotFound,
                str::stream() << "lock with name " << name << " not found"};
    }

    BSONObj doc = findResultSet.front();
    auto locksTypeResult = LocksType::fromBSON(doc);
    if (!locksTypeResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "failed to parse: " << doc << " : "
                              << locksTypeResult.getStatus().toString()};
    }

    return locksTypeResult.getValue();
}

Status DistLockCatalogImpl::stopPing(StringData processId) {
    auto request =
        FindAndModifyRequest::makeRemove(_lockPingNS, BSON(LockpingsType::process() << processId));
    request.setWriteConcern(_writeConcern);

    auto resultStatus = _client->runCommandWithNotMasterRetries(
        "config", _locksNS.db().toString(), request.toBSON());

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    BSONObj responseObj(resultStatus.getValue());

    auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
    return findAndModifyStatus.getStatus();
}

}  // namespace mongo
