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

#include "mongo/s/catalog/replset/dist_lock_catalog_impl.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const char kFindAndModifyResponseResultDocField[] = "value";
const char kLocalTimeField[] = "localTime";
const ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly, TagSet());
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by this mongod.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

const WriteConcernOptions kLocalWriteConcern(1,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             Milliseconds(0));

/**
 * Returns the resulting new object from the findAndModify response object.
 * Returns LockStateChangeFailed if value field was null, which indicates that
 * the findAndModify command did not modify any document.
 * This also checks for errors in the response object.
 */
StatusWith<BSONObj> extractFindAndModifyNewObj(StatusWith<Shard::CommandResponse> response) {
    if (!response.isOK()) {
        return response.getStatus();
    }
    if (!response.getValue().commandStatus.isOK()) {
        return response.getValue().commandStatus;
    }
    if (!response.getValue().writeConcernStatus.isOK()) {
        return response.getValue().writeConcernStatus;
    }

    auto responseObj = std::move(response.getValue().response);

    if (const auto& newDocElem = responseObj[kFindAndModifyResponseResultDocField]) {
        if (newDocElem.isNull()) {
            return {ErrorCodes::LockStateChangeFailed,
                    "findAndModify query predicate didn't match any lock document"};
        }

        if (!newDocElem.isABSONObj()) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "expected an object from the findAndModify response '"
                                  << kFindAndModifyResponseResultDocField
                                  << "'field, got: "
                                  << newDocElem};
        }

        return newDocElem.Obj().getOwned();
    }

    return {ErrorCodes::UnsupportedFormat,
            str::stream() << "no '" << kFindAndModifyResponseResultDocField
                          << "' in findAndModify response"};
}

/**
 * Extract the electionId from a serverStatus command response.
 */
StatusWith<OID> extractElectionId(const BSONObj& responseObj) {
    BSONElement replElem;
    auto replElemStatus = bsonExtractTypedField(responseObj, "repl", Object, &replElem);

    if (!replElemStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, replElemStatus.reason()};
    }

    OID electionId;

    auto electionIdStatus = bsonExtractOIDField(replElem.Obj(), "electionId", &electionId);

    if (!electionIdStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat, electionIdStatus.reason()};
    }

    return electionId;
}

}  // unnamed namespace

DistLockCatalogImpl::DistLockCatalogImpl(ShardRegistry* shardRegistry)
    : _client(shardRegistry), _lockPingNS(LockpingsType::ConfigNS), _locksNS(LocksType::ConfigNS) {}

DistLockCatalogImpl::~DistLockCatalogImpl() = default;

StatusWith<LockpingsType> DistLockCatalogImpl::getPing(OperationContext* txn,
                                                       StringData processID) {
    auto findResult = _findOnConfig(
        txn, kReadPref, _lockPingNS, BSON(LockpingsType::process() << processID), BSONObj(), 1);

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

Status DistLockCatalogImpl::ping(OperationContext* txn, StringData processID, Date_t ping) {
    auto request =
        FindAndModifyRequest::makeUpdate(_lockPingNS,
                                         BSON(LockpingsType::process() << processID),
                                         BSON("$set" << BSON(LockpingsType::ping(ping))));
    request.setUpsert(true);
    request.setWriteConcern(kMajorityWriteConcern);

    auto resultStatus =
        _client->getConfigShard()->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              _locksNS.db().toString(),
                                              request.toBSON(),
                                              Shard::RetryPolicy::kNotIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
    return findAndModifyStatus.getStatus();
}

StatusWith<LocksType> DistLockCatalogImpl::grabLock(OperationContext* txn,
                                                    StringData lockID,
                                                    const OID& lockSessionID,
                                                    StringData who,
                                                    StringData processId,
                                                    Date_t time,
                                                    StringData why) {
    BSONObj newLockDetails(BSON(
        LocksType::lockID(lockSessionID) << LocksType::state(LocksType::LOCKED) << LocksType::who()
                                         << who
                                         << LocksType::process()
                                         << processId
                                         << LocksType::when(time)
                                         << LocksType::why()
                                         << why));

    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::name() << lockID << LocksType::state(LocksType::UNLOCKED)),
        BSON("$set" << newLockDetails));
    request.setUpsert(true);
    request.setShouldReturnNew(true);
    request.setWriteConcern(kMajorityWriteConcern);

    auto resultStatus = _client->getConfigShard()->runCommand(
        txn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::RetryPolicy::kNoRetry);  // Dist lock manager is handling own retries

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
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

StatusWith<LocksType> DistLockCatalogImpl::overtakeLock(OperationContext* txn,
                                                        StringData lockID,
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

    BSONObj newLockDetails(BSON(
        LocksType::lockID(lockSessionID) << LocksType::state(LocksType::LOCKED) << LocksType::who()
                                         << who
                                         << LocksType::process()
                                         << processId
                                         << LocksType::when(time)
                                         << LocksType::why()
                                         << why));

    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS, BSON("$or" << orQueryBuilder.arr()), BSON("$set" << newLockDetails));
    request.setShouldReturnNew(true);
    request.setWriteConcern(kMajorityWriteConcern);

    auto resultStatus =
        _client->getConfigShard()->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              _locksNS.db().toString(),
                                              request.toBSON(),
                                              Shard::RetryPolicy::kNotIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
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

Status DistLockCatalogImpl::unlock(OperationContext* txn, const OID& lockSessionID) {
    auto request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::lockID(lockSessionID)),
        BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
    request.setWriteConcern(kMajorityWriteConcern);

    auto resultStatus =
        _client->getConfigShard()->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              _locksNS.db().toString(),
                                              request.toBSON(),
                                              Shard::RetryPolicy::kIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
    if (findAndModifyStatus == ErrorCodes::LockStateChangeFailed) {
        // Did not modify any document, which implies that the lock already has a
        // a different owner. This is ok since it means that the objective of
        // releasing ownership of the lock has already been accomplished.
        return Status::OK();
    }

    return findAndModifyStatus.getStatus();
}

Status DistLockCatalogImpl::unlockAll(OperationContext* txn, const std::string& processID) {
    std::unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());
    updateDoc->setQuery(BSON(LocksType::process(processID)));
    updateDoc->setUpdateExpr(BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
    updateDoc->setUpsert(false);
    updateDoc->setMulti(true);

    std::unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());

    BatchedCommandRequest request(updateRequest.release());
    request.setNS(_locksNS);
    request.setWriteConcern(kLocalWriteConcern.toBSON());

    BSONObj cmdObj = request.toBSON();

    auto response =
        _client->getConfigShard()->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              _locksNS.db().toString(),
                                              cmdObj,
                                              Shard::RetryPolicy::kIdempotent);

    if (!response.isOK()) {
        return response.getStatus();
    }
    if (!response.getValue().commandStatus.isOK()) {
        return response.getValue().commandStatus;
    }
    if (!response.getValue().writeConcernStatus.isOK()) {
        return response.getValue().writeConcernStatus;
    }

    BatchedCommandResponse batchResponse;
    std::string errmsg;
    if (!batchResponse.parseBSON(response.getValue().response, &errmsg)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Failed to parse config server response to batch request for "
                             "unlocking existing distributed locks"
                          << causedBy(errmsg));
    }
    return batchResponse.toStatus();
}

StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogImpl::getServerInfo(OperationContext* txn) {
    auto resultStatus = _client->getConfigShard()->runCommand(
        txn, kReadPref, "admin", BSON("serverStatus" << 1), Shard::RetryPolicy::kIdempotent);

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }
    if (!resultStatus.getValue().commandStatus.isOK()) {
        return resultStatus.getValue().commandStatus;
    }

    BSONObj responseObj(std::move(resultStatus.getValue().response));

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

StatusWith<LocksType> DistLockCatalogImpl::getLockByTS(OperationContext* txn,
                                                       const OID& lockSessionID) {
    auto findResult = _findOnConfig(
        txn, kReadPref, _locksNS, BSON(LocksType::lockID(lockSessionID)), BSONObj(), 1);

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

StatusWith<LocksType> DistLockCatalogImpl::getLockByName(OperationContext* txn, StringData name) {
    auto findResult =
        _findOnConfig(txn, kReadPref, _locksNS, BSON(LocksType::name() << name), BSONObj(), 1);

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

Status DistLockCatalogImpl::stopPing(OperationContext* txn, StringData processId) {
    auto request =
        FindAndModifyRequest::makeRemove(_lockPingNS, BSON(LockpingsType::process() << processId));
    request.setWriteConcern(kMajorityWriteConcern);

    auto resultStatus =
        _client->getConfigShard()->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              _locksNS.db().toString(),
                                              request.toBSON(),
                                              Shard::RetryPolicy::kNotIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
    return findAndModifyStatus.getStatus();
}

StatusWith<vector<BSONObj>> DistLockCatalogImpl::_findOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto result = _client->getConfigShard()->exhaustiveFindOnConfig(
        txn, readPref, repl::ReadConcernLevel::kMajorityReadConcern, nss, query, sort, limit);
    if (!result.isOK()) {
        return result.getStatus();
    }

    return result.getValue().docs;
}

}  // namespace mongo
