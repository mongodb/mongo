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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"

#include <pcrecpp.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FP_DECLARE(setDropCollDistLockWait);

using repl::OpTime;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using str::stream;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredSelector(ReadPreference::PrimaryPreferred,
                                                            TagSet{});

const int kMaxConfigVersionInitRetry = 3;
const int kMaxReadRetry = 3;

void _toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
}

}  // namespace

CatalogManagerReplicaSet::CatalogManagerReplicaSet(std::unique_ptr<DistLockManager> distLockManager)
    : _distLockManager(std::move(distLockManager)) {}

CatalogManagerReplicaSet::~CatalogManagerReplicaSet() = default;

Status CatalogManagerReplicaSet::startup(OperationContext* txn, bool allowNetworking) {
    _distLockManager->startUp();
    return Status::OK();
}

void CatalogManagerReplicaSet::shutDown(OperationContext* txn, bool allowNetworking) {
    invariant(allowNetworking);
    LOG(1) << "CatalogManagerReplicaSet::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    invariant(_distLockManager);
    _distLockManager->shutDown(txn, allowNetworking);
}

Status CatalogManagerReplicaSet::shardCollection(OperationContext* txn,
                                                 const string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 const vector<BSONObj>& initPoints,
                                                 const set<ShardId>& initShardIds) {
    // Lock the collection globally so that no other mongos can try to shard or drop the collection
    // at the same time.
    auto scopedDistLock = getDistLockManager()->lock(txn, ns, "shardCollection");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    auto status = getDatabase(txn, nsToDatabase(ns));
    if (!status.isOK()) {
        return status.getStatus();
    }

    ShardId dbPrimaryShardId = status.getValue().value.getPrimary();
    const auto primaryShard = grid.shardRegistry()->getShard(txn, dbPrimaryShardId);

    {
        // In 3.0 and prior we include this extra safety check that the collection is not getting
        // sharded concurrently by two different mongos instances. It is not 100%-proof, but it
        // reduces the chance that two invocations of shard collection will step on each other's
        // toes.  Now we take the distributed lock so going forward this check won't be necessary
        // but we leave it around for compatibility with other mongoses from 3.0.
        // TODO(spencer): Remove this after 3.2 ships.
        auto countStatus = _runCountCommandOnConfig(
            txn, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)));
        if (!countStatus.isOK()) {
            return countStatus.getStatus();
        }
        if (countStatus.getValue() > 0) {
            return Status(ErrorCodes::AlreadyInitialized,
                          str::stream() << "collection " << ns << " already sharded with "
                                        << countStatus.getValue() << " chunks.");
        }
    }

    // Record start in changelog
    {
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
        collectionDetail.append("collection", ns);
        collectionDetail.append("primary", primaryShard->toString());

        {
            BSONArrayBuilder initialShards(collectionDetail.subarrayStart("initShards"));
            for (const ShardId& shardId : initShardIds) {
                initialShards.append(shardId);
            }
        }

        collectionDetail.append("numChunks", static_cast<int>(initPoints.size() + 1));

        logChange(txn, "shardCollection.start", ns, collectionDetail.obj());
    }

    shared_ptr<ChunkManager> manager(new ChunkManager(ns, fieldsAndOrder, unique));
    manager->createFirstChunks(txn, dbPrimaryShardId, &initPoints, &initShardIds);
    manager->loadExistingRanges(txn, nullptr);

    CollectionInfo collInfo;
    collInfo.useChunkManager(manager);
    collInfo.save(txn, ns);
    manager->reload(txn, true);

    // Tell the primary mongod to refresh its data
    // TODO:  Think the real fix here is for mongos to just
    //        assume that all collections are sharded, when we get there
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        grid.shardRegistry()->getConfigServerConnectionString(),
        dbPrimaryShardId,
        primaryShard->getConnString(),
        NamespaceString(ns),
        manager->getVersion(),
        true);

    auto ssvStatus = grid.shardRegistry()->runCommandWithNotMasterRetries(
        txn, dbPrimaryShardId, "admin", ssv.toBSON());
    if (!ssvStatus.isOK()) {
        warning() << "could not update initial version of " << ns << " on shard primary "
                  << dbPrimaryShardId << ssvStatus.getStatus();
    }

    logChange(txn, "shardCollection.end", ns, BSON("version" << manager->getVersion().toString()));

    return Status::OK();
}

StatusWith<ShardDrainingStatus> CatalogManagerReplicaSet::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    // Check preconditions for removing the shard
    auto countStatus = _runCountCommandOnConfig(
        txn,
        NamespaceString(ShardType::ConfigNS),
        BSON(ShardType::name() << NE << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() > 0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't have more than one draining shard at a time");
    }

    countStatus = _runCountCommandOnConfig(
        txn, NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << name));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    // Figure out if shard is already draining
    countStatus =
        _runCountCommandOnConfig(txn,
                                 NamespaceString(ShardType::ConfigNS),
                                 BSON(ShardType::name() << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        log() << "going to start draining shard: " << name;

        Status status = update(txn,
                               ShardType::ConfigNS,
                               BSON(ShardType::name() << name),
                               BSON("$set" << BSON(ShardType::draining(true))),
                               false,  // upsert
                               false,  // multi
                               NULL);
        if (!status.isOK()) {
            log() << "error starting removeShard: " << name << "; err: " << status.reason();
            return status;
        }

        grid.shardRegistry()->reload(txn);

        // Record start in changelog
        logChange(txn, "removeShard.start", "", BSON("shard" << name));
        return ShardDrainingStatus::STARTED;
    }

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    countStatus = _runCountCommandOnConfig(
        txn, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long chunkCount = countStatus.getValue();

    countStatus = _runCountCommandOnConfig(
        txn, NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long databaseCount = countStatus.getValue();

    if (chunkCount > 0 || databaseCount > 0) {
        // Still more draining to do
        return ShardDrainingStatus::ONGOING;
    }

    // Draining is done, now finish removing the shard.
    log() << "going to remove shard: " << name;
    audit::logRemoveShard(txn->getClient(), name);

    Status status = remove(txn, ShardType::ConfigNS, BSON(ShardType::name() << name), 0, NULL);
    if (!status.isOK()) {
        log() << "Error concluding removeShard operation on: " << name
              << "; err: " << status.reason();
        return status;
    }

    grid.shardRegistry()->remove(name);
    grid.shardRegistry()->reload(txn);

    // Record finish in changelog
    logChange(txn, "removeShard", "", BSON("shard" << name));

    return ShardDrainingStatus::COMPLETED;
}

StatusWith<OpTimePair<DatabaseType>> CatalogManagerReplicaSet::getDatabase(
    OperationContext* txn, const std::string& dbName) {
    if (!NamespaceString::validDBName(dbName)) {
        return {ErrorCodes::InvalidNamespace, stream() << dbName << " is not a valid db name"};
    }

    // The two databases that are hosted on the config server are config and admin
    if (dbName == "config" || dbName == "admin") {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setSharded(false);
        dbt.setPrimary("config");

        return OpTimePair<DatabaseType>(dbt);
    }

    auto result = _fetchDatabaseMetadata(txn, dbName, kConfigReadSelector);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        result =
            _fetchDatabaseMetadata(txn, dbName, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        if (!result.isOK() && (result != ErrorCodes::NamespaceNotFound)) {
            return Status(result.getStatus().code(),
                          str::stream() << "Could not confirm non-existence of database \""
                                        << dbName
                                        << "\" due to inability to query the config server primary"
                                        << causedBy(result.getStatus()));
        }
    }

    return result;
}

StatusWith<OpTimePair<DatabaseType>> CatalogManagerReplicaSet::_fetchDatabaseMetadata(
    OperationContext* txn, const std::string& dbName, const ReadPreferenceSetting& readPref) {
    dassert(dbName != "admin" && dbName != "config");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              readPref,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::name(dbName)),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsWithOpTime = findStatus.getValue();
    if (docsWithOpTime.value.empty()) {
        return {ErrorCodes::NamespaceNotFound, stream() << "database " << dbName << " not found"};
    }

    invariant(docsWithOpTime.value.size() == 1);

    auto parseStatus = DatabaseType::fromBSON(docsWithOpTime.value.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    return OpTimePair<DatabaseType>(parseStatus.getValue(), docsWithOpTime.opTime);
}

StatusWith<OpTimePair<CollectionType>> CatalogManagerReplicaSet::getCollection(
    OperationContext* txn, const std::string& collNs) {
    auto configShard = grid.shardRegistry()->getShard(txn, "config");

    auto statusFind = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(CollectionType::ConfigNS),
                                              BSON(CollectionType::fullNs(collNs)),
                                              BSONObj(),
                                              1);
    if (!statusFind.isOK()) {
        return statusFind.getStatus();
    }

    const auto& retOpTimePair = statusFind.getValue();
    const auto& retVal = retOpTimePair.value;
    if (retVal.empty()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << collNs << " not found");
    }

    invariant(retVal.size() == 1);

    auto parseStatus = CollectionType::fromBSON(retVal.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    return OpTimePair<CollectionType>(parseStatus.getValue(), retOpTimePair.opTime);
}

Status CatalogManagerReplicaSet::getCollections(OperationContext* txn,
                                                const std::string* dbName,
                                                std::vector<CollectionType>* collections,
                                                OpTime* opTime) {
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      string(str::stream() << "^" << pcrecpp::RE::QuoteMeta(*dbName) << "\\."));
    }

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(CollectionType::ConfigNS),
                                              b.obj(),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsOpTimePair = findStatus.getValue();

    for (const BSONObj& obj : docsOpTimePair.value) {
        const auto collectionResult = CollectionType::fromBSON(obj);
        if (!collectionResult.isOK()) {
            collections->clear();
            return {ErrorCodes::FailedToParse,
                    str::stream() << "error while parsing " << CollectionType::ConfigNS
                                  << " document: " << obj << " : "
                                  << collectionResult.getStatus().toString()};
        }

        collections->push_back(collectionResult.getValue());
    }

    if (opTime) {
        *opTime = docsOpTimePair.opTime;
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::dropCollection(OperationContext* txn, const NamespaceString& ns) {
    logChange(txn, "dropCollection.start", ns.ns(), BSONObj());

    auto shardsStatus = getAllShards(txn);
    if (!shardsStatus.isOK()) {
        return shardsStatus.getStatus();
    }
    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    LOG(1) << "dropCollection " << ns << " started";

    // Lock the collection globally so that split/migrate cannot run
    stdx::chrono::seconds waitFor(2);
    MONGO_FAIL_POINT_BLOCK(setDropCollDistLockWait, customWait) {
        const BSONObj& data = customWait.getData();
        waitFor = stdx::chrono::seconds(data["waitForSecs"].numberInt());
    }
    const stdx::chrono::milliseconds lockTryInterval(500);
    auto scopedDistLock =
        getDistLockManager()->lock(txn, ns.ns(), "drop", waitFor, lockTryInterval);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    LOG(1) << "dropCollection " << ns << " locked";

    std::map<string, BSONObj> errors;
    auto* shardRegistry = grid.shardRegistry();

    for (const auto& shardEntry : allShards) {
        auto dropResult = shardRegistry->runCommandWithNotMasterRetries(
            txn, shardEntry.getName(), ns.db().toString(), BSON("drop" << ns.coll()));

        if (!dropResult.isOK()) {
            return dropResult.getStatus();
        }

        auto dropStatus = getStatusFromCommandResult(dropResult.getValue());
        if (!dropStatus.isOK()) {
            if (dropStatus.code() == ErrorCodes::NamespaceNotFound) {
                continue;
            }

            errors.emplace(shardEntry.getHost(), dropResult.getValue());
        }
    }

    if (!errors.empty()) {
        StringBuilder sb;
        sb << "Dropping collection failed on the following hosts: ";

        for (auto it = errors.cbegin(); it != errors.cend(); ++it) {
            if (it != errors.cbegin()) {
                sb << ", ";
            }

            sb << it->first << ": " << it->second;
        }

        return {ErrorCodes::OperationFailed, sb.str()};
    }

    LOG(1) << "dropCollection " << ns << " shard data deleted";

    // Remove chunk data
    Status result = remove(txn, ChunkType::ConfigNS, BSON(ChunkType::ns(ns.ns())), 0, nullptr);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " chunk data deleted";

    // Mark the collection as dropped
    CollectionType coll;
    coll.setNs(ns);
    coll.setDropped(true);
    coll.setEpoch(ChunkVersion::DROPPED().epoch());
    coll.setUpdatedAt(grid.shardRegistry()->getNetwork()->now());

    result = updateCollection(txn, ns.ns(), coll);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " collection marked as dropped";

    for (const auto& shardEntry : allShards) {
        SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
            grid.shardRegistry()->getConfigServerConnectionString(),
            shardEntry.getName(),
            fassertStatusOK(28781, ConnectionString::parse(shardEntry.getHost())),
            ns,
            ChunkVersion::DROPPED(),
            true);

        auto ssvResult = shardRegistry->runCommandWithNotMasterRetries(
            txn, shardEntry.getName(), "admin", ssv.toBSON());

        if (!ssvResult.isOK()) {
            return ssvResult.getStatus();
        }

        auto ssvStatus = getStatusFromCommandResult(ssvResult.getValue());
        if (!ssvStatus.isOK()) {
            return ssvStatus;
        }

        auto unsetShardingStatus = shardRegistry->runCommandWithNotMasterRetries(
            txn, shardEntry.getName(), "admin", BSON("unsetSharding" << 1));

        if (!unsetShardingStatus.isOK()) {
            return unsetShardingStatus.getStatus();
        }

        auto unsetShardingResult = getStatusFromCommandResult(unsetShardingStatus.getValue());
        if (!unsetShardingResult.isOK()) {
            return unsetShardingResult;
        }
    }

    LOG(1) << "dropCollection " << ns << " completed";

    logChange(txn, "dropCollection", ns.ns(), BSONObj());

    return Status::OK();
}

StatusWith<SettingsType> CatalogManagerReplicaSet::getGlobalSettings(OperationContext* txn,
                                                                     const string& key) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(SettingsType::ConfigNS),
                                              BSON(SettingsType::key(key)),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "can't find settings document with key: " << key};
    }

    BSONObj settingsDoc = docs.front();
    StatusWith<SettingsType> settingsResult = SettingsType::fromBSON(settingsDoc);
    if (!settingsResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "error while parsing settings document: " << settingsDoc << " : "
                              << settingsResult.getStatus().toString()};
    }

    const SettingsType& settings = settingsResult.getValue();

    Status validationStatus = settings.validate();
    if (!validationStatus.isOK()) {
        return validationStatus;
    }

    return settingsResult;
}

Status CatalogManagerReplicaSet::getDatabasesForShard(OperationContext* txn,
                                                      const string& shardName,
                                                      vector<string>* dbs) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::primary(shardName)),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& obj : findStatus.getValue().value) {
        string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            dbs->clear();
            return status;
        }

        dbs->push_back(dbName);
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::getChunks(OperationContext* txn,
                                           const BSONObj& query,
                                           const BSONObj& sort,
                                           boost::optional<int> limit,
                                           vector<ChunkType>* chunks,
                                           OpTime* opTime) {
    chunks->clear();

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(
        txn, kConfigReadSelector, NamespaceString(ChunkType::ConfigNS), query, sort, longLimit);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto chunkDocsOpTimePair = findStatus.getValue();
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
        auto chunkRes = ChunkType::fromBSON(obj);
        if (!chunkRes.isOK()) {
            chunks->clear();
            return {ErrorCodes::FailedToParse,
                    stream() << "Failed to parse chunk with id ("
                             << obj[ChunkType::name()].toString()
                             << "): " << chunkRes.getStatus().toString()};
        }

        chunks->push_back(chunkRes.getValue());
    }

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::getTagsForCollection(OperationContext* txn,
                                                      const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
    tags->clear();

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns(collectionNs)),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }
    for (const BSONObj& obj : findStatus.getValue().value) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            tags->clear();
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Failed to parse tag: " << tagRes.getStatus().toString());
        }

        tags->push_back(tagRes.getValue());
    }

    return Status::OK();
}

StatusWith<string> CatalogManagerReplicaSet::getTagForChunk(OperationContext* txn,
                                                            const std::string& collectionNs,
                                                            const ChunkType& chunk) {
    BSONObj query =
        BSON(TagsType::ns(collectionNs) << TagsType::min() << BSON("$lte" << chunk.getMin())
                                        << TagsType::max() << BSON("$gte" << chunk.getMax()));
    auto findStatus = _exhaustiveFindOnConfig(
        txn, kConfigReadSelector, NamespaceString(TagsType::ConfigNS), query, BSONObj(), 1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return string{};
    }

    invariant(docs.size() == 1);
    BSONObj tagsDoc = docs.front();

    const auto tagsResult = TagsType::fromBSON(tagsDoc);
    if (!tagsResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                stream() << "error while parsing " << TagsType::ConfigNS << " document: " << tagsDoc
                         << " : " << tagsResult.getStatus().toString()};
    }
    return tagsResult.getValue().getTag();
}

StatusWith<OpTimePair<std::vector<ShardType>>> CatalogManagerReplicaSet::getAllShards(
    OperationContext* txn) {
    std::vector<ShardType> shards;
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(ShardType::ConfigNS),
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().value) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            shards.clear();
            return {ErrorCodes::FailedToParse,
                    stream() << "Failed to parse shard with id ("
                             << doc[ShardType::name()].toString() << ")"
                             << causedBy(shardRes.getStatus())};
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return {validateStatus.code(),
                    stream() << "Failed to validate shard with id ("
                             << doc[ShardType::name()].toString() << ")"
                             << causedBy(validateStatus)};
        }

        shards.push_back(shardRes.getValue());
    }

    return OpTimePair<std::vector<ShardType>>{std::move(shards), findStatus.getValue().opTime};
}

bool CatalogManagerReplicaSet::runUserManagementWriteCommand(OperationContext* txn,
                                                             const std::string& commandName,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    BSONObj cmdToRun = cmdObj;
    {
        // Make sure that if the command has a write concern that it is w:1 or w:majority, and
        // convert w:1 or no write concern to w:majority before sending.
        WriteConcernOptions writeConcern;
        const char* writeConcernFieldName = "writeConcern";
        BSONElement writeConcernElement = cmdObj[writeConcernFieldName];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            Status status = writeConcern.parse(writeConcernElement.Obj());
            if (!status.isOK()) {
                return Command::appendCommandStatus(*result, status);
            }
            if (!writeConcern.validForConfigServers()) {
                return Command::appendCommandStatus(
                    *result,
                    Status(ErrorCodes::InvalidOptions,
                           str::stream()
                               << "Invalid replication write concern.  Writes to config server "
                                  "replica sets must use w:'majority', got: "
                               << writeConcern.toBSON()));
            }
        }
        writeConcern.wMode = WriteConcernOptions::kMajority;
        writeConcern.wNumNodes = 0;

        BSONObjBuilder modifiedCmd;
        if (!initialCmdHadWriteConcern) {
            modifiedCmd.appendElements(cmdObj);
        } else {
            BSONObjIterator cmdObjIter(cmdObj);
            while (cmdObjIter.more()) {
                BSONElement e = cmdObjIter.next();
                if (str::equals(e.fieldName(), writeConcernFieldName)) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(writeConcernFieldName, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto response =
        grid.shardRegistry()->runCommandOnConfigWithNotMasterRetries(txn, dbname, cmdToRun);

    if (!response.isOK()) {
        return Command::appendCommandStatus(*result, response.getStatus());
    }

    result->appendElements(response.getValue());
    return Command::getStatusFromCommandResult(response.getValue()).isOK();
}

bool CatalogManagerReplicaSet::runReadCommandForTest(OperationContext* txn,
                                                     const std::string& dbname,
                                                     const BSONObj& cmdObj,
                                                     BSONObjBuilder* result) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.appendElements(cmdObj);
    _appendReadConcern(&cmdBuilder);

    auto resultStatus = _runReadCommand(txn, dbname, cmdBuilder.done(), kConfigReadSelector);
    if (resultStatus.isOK()) {
        result->appendElements(resultStatus.getValue());
        return Command::getStatusFromCommandResult(resultStatus.getValue()).isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

bool CatalogManagerReplicaSet::runUserManagementReadCommand(OperationContext* txn,
                                                            const std::string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
    auto resultStatus = _runReadCommand(txn, dbname, cmdObj, kConfigPrimaryPreferredSelector);
    if (resultStatus.isOK()) {
        result->appendElements(resultStatus.getValue());
        return Command::getStatusFromCommandResult(resultStatus.getValue()).isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

Status CatalogManagerReplicaSet::applyChunkOpsDeprecated(OperationContext* txn,
                                                         const BSONArray& updateOps,
                                                         const BSONArray& preCondition) {
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition);
    auto response =
        grid.shardRegistry()->runCommandOnConfigWithNotMasterRetries(txn, "config", cmd);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = Command::getStatusFromCommandResult(response.getValue());
    if (!status.isOK()) {
        string errMsg(str::stream() << "Unable to save chunk ops. Command: " << cmd
                                    << ". Result: " << response.getValue());

        return Status(status.code(), errMsg);
    }
    return Status::OK();
}

DistLockManager* CatalogManagerReplicaSet::getDistLockManager() {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void CatalogManagerReplicaSet::writeConfigServerDirect(OperationContext* txn,
                                                       const BatchedCommandRequest& batchRequest,
                                                       BatchedCommandResponse* batchResponse) {
    std::string dbname = batchRequest.getNS().db().toString();
    invariant(dbname == "config" || dbname == "admin");
    const BSONObj cmdObj = batchRequest.toBSON();

    auto response =
        grid.shardRegistry()->runCommandOnConfigWithNotMasterRetries(txn, dbname, cmdObj);
    if (!response.isOK()) {
        _toBatchError(response.getStatus(), batchResponse);
        return;
    }

    string errmsg;
    if (!batchResponse->parseBSON(response.getValue(), &errmsg)) {
        _toBatchError(Status(ErrorCodes::FailedToParse,
                             str::stream() << "Failed to parse config server response: " << errmsg),
                      batchResponse);
    }
}

Status CatalogManagerReplicaSet::insertConfigDocument(OperationContext* txn,
                                                      const std::string& ns,
                                                      const BSONObj& doc) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config");
    invariant(doc.hasField("_id"));

    auto insert(stdx::make_unique<BatchedInsertRequest>());
    insert->addToDocuments(doc);

    BatchedCommandRequest request(insert.release());
    request.setNS(nss);
    request.setWriteConcern(WriteConcernOptions::Majority);

    BatchedCommandResponse response;
    writeConfigServerDirect(txn, request, &response);

    return response.toStatus();
}

Status CatalogManagerReplicaSet::_checkDbDoesNotExist(OperationContext* txn,
                                                      const string& dbName,
                                                      DatabaseType* db) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              queryBuilder.obj(),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return Status::OK();
    }

    BSONObj dbObj = docs.front();
    std::string actualDbName = dbObj[DatabaseType::name()].String();
    if (actualDbName == dbName) {
        if (db) {
            auto parseDBStatus = DatabaseType::fromBSON(dbObj);
            if (!parseDBStatus.isOK()) {
                return parseDBStatus.getStatus();
            }

            *db = parseDBStatus.getValue();
        }

        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "database " << dbName << " already exists");
    }

    return Status(ErrorCodes::DatabaseDifferCase,
                  str::stream() << "can't have 2 databases that just differ on case "
                                << " have: " << actualDbName << " want to add: " << dbName);
}

StatusWith<std::string> CatalogManagerReplicaSet::_generateNewShardName(OperationContext* txn) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(ShardType::ConfigNS),
                                              shardNameRegex.obj(),
                                              BSON(ShardType::name() << -1),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;

    int count = 0;
    if (!docs.empty()) {
        const auto shardStatus = ShardType::fromBSON(docs.front());
        if (!shardStatus.isOK()) {
            return shardStatus.getStatus();
        }

        std::istringstream is(shardStatus.getValue().getName().substr(5));
        is >> count;
        count++;
    }

    // TODO fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

Status CatalogManagerReplicaSet::_createCappedConfigCollection(OperationContext* txn,
                                                               StringData collName,
                                                               int cappedSize) {
    BSONObj createCmd = BSON("create" << collName << "capped" << true << "size" << cappedSize);
    auto result =
        grid.shardRegistry()->runCommandOnConfigWithNotMasterRetries(txn, "config", createCmd);
    if (!result.isOK()) {
        return result.getStatus();
    }

    return Command::getStatusFromCommandResult(result.getValue());
}

StatusWith<long long> CatalogManagerReplicaSet::_runCountCommandOnConfig(OperationContext* txn,
                                                                         const NamespaceString& ns,
                                                                         BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ns.coll());
    countBuilder.append("query", query);
    _appendReadConcern(&countBuilder);

    auto resultStatus =
        _runReadCommand(txn, ns.db().toString(), countBuilder.done(), kConfigReadSelector);
    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    auto responseObj = std::move(resultStatus.getValue());

    long long result;
    auto status = bsonExtractIntegerField(responseObj, "n", &result);
    if (!status.isOK()) {
        return status;
    }

    return result;
}

Status CatalogManagerReplicaSet::initConfigVersion(OperationContext* txn) {
    for (int x = 0; x < kMaxConfigVersionInitRetry; x++) {
        auto versionStatus = _getConfigVersion(txn);
        if (!versionStatus.isOK()) {
            return versionStatus.getStatus();
        }

        auto versionInfo = versionStatus.getValue();
        if (versionInfo.getMinCompatibleVersion() > CURRENT_CONFIG_VERSION) {
            return {ErrorCodes::IncompatibleShardingConfigVersion,
                    str::stream() << "current version v" << CURRENT_CONFIG_VERSION
                                  << " is older than the cluster min compatible v"
                                  << versionInfo.getMinCompatibleVersion()};
        }

        if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
            VersionType newVersion;
            newVersion.setClusterId(OID::gen());
            newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
            newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

            BSONObj versionObj(newVersion.toBSON());
            BatchedCommandResponse response;
            auto upsertStatus = update(txn,
                                       VersionType::ConfigNS,
                                       versionObj,
                                       versionObj,
                                       true /* upsert*/,
                                       false /* multi */,
                                       &response);

            if ((upsertStatus.isOK() && response.getN() < 1) ||
                upsertStatus == ErrorCodes::DuplicateKey) {
                // Do the check again as someone inserted a new config version document
                // and the upsert neither inserted nor updated a config version document.
                // Note: you can get duplicate key errors on upsert because of SERVER-14322.
                continue;
            }

            return upsertStatus;
        }

        if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
            return {ErrorCodes::IncompatibleShardingConfigVersion,
                    "Assuming config data is old since the version document cannot be found in the "
                    "config server and it contains databases aside 'local' and 'admin'. "
                    "Please upgrade if this is the case. Otherwise, make sure that the config "
                    "server is clean."};
        }

        if (versionInfo.getCurrentVersion() < CURRENT_CONFIG_VERSION) {
            return {ErrorCodes::IncompatibleShardingConfigVersion,
                    str::stream() << "need to upgrade current cluster version to v"
                                  << CURRENT_CONFIG_VERSION << "; currently at v"
                                  << versionInfo.getCurrentVersion()};
        }

        return Status::OK();
    }

    return {ErrorCodes::IncompatibleShardingConfigVersion,
            str::stream() << "unable to create new config version document after "
                          << kMaxConfigVersionInitRetry << " retries"};
}

StatusWith<VersionType> CatalogManagerReplicaSet::_getConfigVersion(OperationContext* txn) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(VersionType::ConfigNS),
                                              BSONObj(),
                                              BSONObj(),
                                              boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().value;

    if (queryResults.size() > 1) {
        return {ErrorCodes::RemoteValidationError,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS};
    }

    if (queryResults.empty()) {
        auto countStatus =
            _runCountCommandOnConfig(txn, NamespaceString(ShardType::ConfigNS), BSONObj());

        if (!countStatus.isOK()) {
            return countStatus.getStatus();
        }

        const auto& shardCount = countStatus.getValue();
        if (shardCount > 0) {
            // Version document doesn't exist, but config.shards is not empty. Assuming that
            // the current config metadata is pre v2.4.
            VersionType versionInfo;
            versionInfo.setMinCompatibleVersion(UpgradeHistory_UnreportedVersion);
            versionInfo.setCurrentVersion(UpgradeHistory_UnreportedVersion);
            return versionInfo;
        }

        VersionType versionInfo;
        versionInfo.setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setCurrentVersion(UpgradeHistory_EmptyVersion);
        return versionInfo;
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "invalid config version document: " << versionDoc
                                    << versionTypeResult.getStatus().toString());
    }

    return versionTypeResult.getValue();
}

StatusWith<OpTimePair<vector<BSONObj>>> CatalogManagerReplicaSet::_exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto response =
        grid.shardRegistry()->exhaustiveFindOnConfig(txn, readPref, nss, query, sort, limit);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return OpTimePair<vector<BSONObj>>(std::move(response.getValue().docs),
                                       response.getValue().opTime);
}

void CatalogManagerReplicaSet::_appendReadConcern(BSONObjBuilder* builder) {
    repl::ReadConcernArgs readConcern(grid.shardRegistry()->getConfigOpTime(),
                                      repl::ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(builder);
}

StatusWith<BSONObj> CatalogManagerReplicaSet::_runReadCommand(
    OperationContext* txn,
    const std::string& dbname,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& settings) {
    for (int retry = 1; retry <= kMaxReadRetry; ++retry) {
        auto response = grid.shardRegistry()->runCommandOnConfig(txn, settings, dbname, cmdObj);
        if (response.isOK()) {
            return response;
        }

        if (ErrorCodes::isNetworkError(response.getStatus().code()) && retry < kMaxReadRetry) {
            continue;
        }

        return response.getStatus();
    }

    MONGO_UNREACHABLE;
}

Status CatalogManagerReplicaSet::appendInfoForConfigServerDatabases(OperationContext* txn,
                                                                    BSONArrayBuilder* builder) {
    auto resultStatus =
        _runReadCommand(txn, "admin", BSON("listDatabases" << 1), kConfigPrimaryPreferredSelector);

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    auto listDBResponse = resultStatus.getValue();
    BSONElement dbListArray;
    auto dbListStatus = bsonExtractTypedField(listDBResponse, "databases", Array, &dbListArray);
    if (!dbListStatus.isOK()) {
        return dbListStatus;
    }

    BSONObjIterator iter(dbListArray.Obj());

    while (iter.more()) {
        auto dbEntry = iter.next().Obj();
        string name;
        auto parseStatus = bsonExtractStringField(dbEntry, "name", &name);

        if (!parseStatus.isOK()) {
            return parseStatus;
        }

        if (name == "config" || name == "admin") {
            builder->append(dbEntry);
        }
    }

    return Status::OK();
}

}  // namespace mongo
