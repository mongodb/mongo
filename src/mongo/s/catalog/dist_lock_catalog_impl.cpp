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
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const char kFindAndModifyResponseResultDocField[] = "value";
const char kLocalTimeField[] = "localTime";

const ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly, TagSet());

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

    const auto replSubObj = replElem.Obj();
    OID electionId;
    auto electionIdStatus = bsonExtractOIDField(replSubObj, "electionId", &electionId);

    if (!electionIdStatus.isOK()) {
        // Secondaries don't have electionId.
        if (electionIdStatus.code() == ErrorCodes::NoSuchKey) {
            // Verify that the from replSubObj that this is indeed not a primary.
            bool isPrimary = false;
            auto isPrimaryStatus = bsonExtractBooleanField(replSubObj, "ismaster", &isPrimary);

            if (!isPrimaryStatus.isOK()) {
                return {ErrorCodes::UnsupportedFormat, isPrimaryStatus.reason()};
            }

            if (isPrimary) {
                string hostContacted;
                auto hostContactedStatus = bsonExtractStringField(replSubObj, "me", &hostContacted);

                if (!hostContactedStatus.isOK()) {
                    return {
                        ErrorCodes::UnsupportedFormat,
                        str::stream()
                            << "failed to extract 'me' field from repl subsection of serverStatus: "
                            << hostContactedStatus.reason()};
                }

                return {ErrorCodes::UnsupportedFormat,
                        str::stream() << "expected primary to have electionId but not present on "
                                      << hostContacted};
            }

            return {ErrorCodes::NotMaster, "only primary can have electionId"};
        }

        return {ErrorCodes::UnsupportedFormat, electionIdStatus.reason()};
    }

    return electionId;
}

}  // unnamed namespace

DistLockCatalogImpl::DistLockCatalogImpl()
    : _lockPingNS(LockpingsType::ConfigNS), _locksNS(LocksType::ConfigNS) {}

DistLockCatalogImpl::~DistLockCatalogImpl() = default;

StatusWith<LockpingsType> DistLockCatalogImpl::getPing(OperationContext* opCtx,
                                                       StringData processID) {
    auto findResult = _findOnConfig(
        opCtx, kReadPref, _lockPingNS, BSON(LockpingsType::process() << processID), BSONObj(), 1);

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

Status DistLockCatalogImpl::ping(OperationContext* opCtx, StringData processID, Date_t ping) {
    auto request =
        FindAndModifyRequest::makeUpdate(_lockPingNS,
                                         BSON(LockpingsType::process() << processID),
                                         BSON("$set" << BSON(LockpingsType::ping(ping))));
    request.setUpsert(true);
    request.setWriteConcern(kMajorityWriteConcern);

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kNotIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
    return findAndModifyStatus.getStatus();
}

StatusWith<LocksType> DistLockCatalogImpl::grabLock(OperationContext* opCtx,
                                                    StringData lockID,
                                                    const OID& lockSessionID,
                                                    StringData who,
                                                    StringData processId,
                                                    Date_t time,
                                                    StringData why,
                                                    const WriteConcernOptions& writeConcern) {
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
    request.setWriteConcern(writeConcern);

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::kDefaultConfigCommandTimeout,
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

StatusWith<LocksType> DistLockCatalogImpl::overtakeLock(OperationContext* opCtx,
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

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::kDefaultConfigCommandTimeout,
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

Status DistLockCatalogImpl::unlock(OperationContext* opCtx, const OID& lockSessionID) {
    FindAndModifyRequest request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::lockID(lockSessionID)),
        BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
    request.setWriteConcern(kMajorityWriteConcern);
    return _unlock(opCtx, request);
}

Status DistLockCatalogImpl::unlock(OperationContext* opCtx,
                                   const OID& lockSessionID,
                                   StringData name) {
    FindAndModifyRequest request = FindAndModifyRequest::makeUpdate(
        _locksNS,
        BSON(LocksType::lockID(lockSessionID) << LocksType::name(name.toString())),
        BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
    request.setWriteConcern(kMajorityWriteConcern);
    return _unlock(opCtx, request);
}

Status DistLockCatalogImpl::_unlock(OperationContext* opCtx, const FindAndModifyRequest& request) {
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::kDefaultConfigCommandTimeout,
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

Status DistLockCatalogImpl::unlockAll(OperationContext* opCtx, const std::string& processID) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(_locksNS);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON(LocksType::process(processID)));
            entry.setU(BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
            entry.setUpsert(false);
            entry.setMulti(true);
            return entry;
        }()});
        return updateOp;
    }());
    request.setWriteConcern(kLocalWriteConcern.toBSON());

    BSONObj cmdObj = request.toBSON();

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto response = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        cmdObj,
        Shard::kDefaultConfigCommandTimeout,
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

StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogImpl::getServerInfo(
    OperationContext* opCtx) {
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        kReadPref,
        "admin",
        BSON("serverStatus" << 1),
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kIdempotent);

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
        return electionIdStatus.getStatus();
    }

    return DistLockCatalog::ServerInfo(localTimeElem.date(), electionIdStatus.getValue());
}

StatusWith<LocksType> DistLockCatalogImpl::getLockByTS(OperationContext* opCtx,
                                                       const OID& lockSessionID) {
    auto findResult = _findOnConfig(
        opCtx, kReadPref, _locksNS, BSON(LocksType::lockID(lockSessionID)), BSONObj(), 1);

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

StatusWith<LocksType> DistLockCatalogImpl::getLockByName(OperationContext* opCtx, StringData name) {
    auto findResult =
        _findOnConfig(opCtx, kReadPref, _locksNS, BSON(LocksType::name() << name), BSONObj(), 1);

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

Status DistLockCatalogImpl::stopPing(OperationContext* opCtx, StringData processId) {
    auto request =
        FindAndModifyRequest::makeRemove(_lockPingNS, BSON(LockpingsType::process() << processId));
    request.setWriteConcern(kMajorityWriteConcern);

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resultStatus = shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        _locksNS.db().toString(),
        request.toBSON(),
        Shard::kDefaultConfigCommandTimeout,
        Shard::RetryPolicy::kNotIdempotent);

    auto findAndModifyStatus = extractFindAndModifyNewObj(std::move(resultStatus));
    return findAndModifyStatus.getStatus();
}

StatusWith<vector<BSONObj>> DistLockCatalogImpl::_findOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto result = shardRegistry->getConfigShard()->exhaustiveFindOnConfig(
        opCtx, readPref, repl::ReadConcernLevel::kMajorityReadConcern, nss, query, sort, limit);
    if (!result.isOK()) {
        return result.getStatus();
    }

    return result.getValue().docs;
}

}  // namespace mongo
