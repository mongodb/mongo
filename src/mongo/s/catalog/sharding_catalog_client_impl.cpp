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

#include "mongo/s/catalog/sharding_catalog_client_impl.h"

#include <iomanip>
#include <pcrecpp.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(failApplyChunkOps);

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
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;

const std::string kActionLogCollectionName("actionlog");
const int kActionLogCollectionSizeMB = 2 * 1024 * 1024;

const std::string kChangeLogCollectionName("changelog");
const int kChangeLogCollectionSizeMB = 10 * 1024 * 1024;

const NamespaceString kSettingsNamespace("config", "settings");

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(status);
}

}  // namespace

ShardingCatalogClientImpl::ShardingCatalogClientImpl(
    std::unique_ptr<DistLockManager> distLockManager)
    : _distLockManager(std::move(distLockManager)) {}

ShardingCatalogClientImpl::~ShardingCatalogClientImpl() = default;

void ShardingCatalogClientImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return;
    }

    _started = true;
    _distLockManager->startUp();
}

void ShardingCatalogClientImpl::shutDown(OperationContext* opCtx) {
    LOG(1) << "ShardingCatalogClientImpl::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    invariant(_distLockManager);
    _distLockManager->shutDown(opCtx);
}

Status ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionType& coll,
    const bool upsert) {
    fassert(28634, coll.validate());

    auto status = _updateConfigDocument(opCtx,
                                        CollectionType::ConfigNS,
                                        BSON(CollectionType::fullNs(nss.ns())),
                                        coll.toBSON(),
                                        upsert,
                                        ShardingCatalogClient::kMajorityWriteConcern);
    return status.getStatus().withContext(str::stream() << "Collection metadata write failed");
}

Status ShardingCatalogClientImpl::updateDatabase(OperationContext* opCtx,
                                                 const std::string& dbName,
                                                 const DatabaseType& db) {
    fassert(28616, db.validate());

    auto status = updateConfigDocument(opCtx,
                                       DatabaseType::ConfigNS,
                                       BSON(DatabaseType::name(dbName)),
                                       db.toBSON(),
                                       true,
                                       ShardingCatalogClient::kMajorityWriteConcern);
    return status.getStatus().withContext(str::stream() << "Database metadata write failed");
}

Status ShardingCatalogClientImpl::logAction(OperationContext* opCtx,
                                            const std::string& what,
                                            const std::string& ns,
                                            const BSONObj& detail) {
    if (_actionLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(opCtx,
                                                      kActionLogCollectionName,
                                                      kActionLogCollectionSizeMB,
                                                      ShardingCatalogClient::kMajorityWriteConcern);
        if (result.isOK()) {
            _actionLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.actionlog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(opCtx,
                kActionLogCollectionName,
                what,
                ns,
                detail,
                ShardingCatalogClient::kMajorityWriteConcern);
}

Status ShardingCatalogClientImpl::logChange(OperationContext* opCtx,
                                            const std::string& what,
                                            const std::string& ns,
                                            const BSONObj& detail,
                                            const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              writeConcern.wMode == WriteConcernOptions::kMajority);
    if (_changeLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            opCtx, kChangeLogCollectionName, kChangeLogCollectionSizeMB, writeConcern);
        if (result.isOK()) {
            _changeLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.changelog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(opCtx, kChangeLogCollectionName, what, ns, detail, writeConcern);
}

Status ShardingCatalogClientImpl::_log(OperationContext* opCtx,
                                       const StringData& logCollName,
                                       const std::string& what,
                                       const std::string& operationNS,
                                       const BSONObj& detail,
                                       const WriteConcernOptions& writeConcern) {
    Date_t now = Grid::get(opCtx)->getNetwork()->now();
    const std::string hostName = Grid::get(opCtx)->getNetwork()->getHostName();
    const string changeId = str::stream() << hostName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(hostName);
    changeLog.setClientAddr(opCtx->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS);
    changeLog.setWhat(what);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    log() << "about to log metadata event into " << logCollName << ": " << redact(changeLogBSON);

    const NamespaceString nss("config", logCollName);
    Status result = insertConfigDocument(opCtx, nss, changeLogBSON, writeConcern);

    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID [" << changeId
                  << "] into collection " << logCollName << ": " << redact(result);
    }

    return result;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::getDatabase(
    OperationContext* opCtx, const std::string& dbName, repl::ReadConcernLevel readConcernLevel) {
    if (!NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow)) {
        return {ErrorCodes::InvalidNamespace, stream() << dbName << " is not a valid db name"};
    }

    // The admin database is always hosted on the config server.
    if (dbName == "admin") {
        DatabaseType dbt(dbName, ShardRegistry::kConfigServerShardId, false);
        return repl::OpTimeWith<DatabaseType>(dbt);
    }

    // The config database's primary shard is always config, and it is always sharded.
    if (dbName == "config") {
        DatabaseType dbt(dbName, ShardRegistry::kConfigServerShardId, true);
        return repl::OpTimeWith<DatabaseType>(dbt);
    }

    auto result = _fetchDatabaseMetadata(opCtx, dbName, kConfigReadSelector, readConcernLevel);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        result = _fetchDatabaseMetadata(
            opCtx, dbName, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, readConcernLevel);
        if (!result.isOK() && (result != ErrorCodes::NamespaceNotFound)) {
            return result.getStatus().withContext(
                str::stream() << "Could not confirm non-existence of database " << dbName);
        }
    }

    return result;
}

StatusWith<repl::OpTimeWith<std::vector<DatabaseType>>> ShardingCatalogClientImpl::getAllDBs(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    std::vector<DatabaseType> databases;
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              DatabaseType::ConfigNS,
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().value) {
        auto dbRes = DatabaseType::fromBSON(doc);
        if (!dbRes.isOK()) {
            return dbRes.getStatus().withContext(stream() << "Failed to parse database document "
                                                          << doc);
        }

        Status validateStatus = dbRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return validateStatus.withContext(stream() << "Failed to validate database document "
                                                       << doc);
        }

        databases.push_back(dbRes.getValue());
    }

    return repl::OpTimeWith<std::vector<DatabaseType>>{std::move(databases),
                                                       findStatus.getValue().opTime};
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::_fetchDatabaseMetadata(
    OperationContext* opCtx,
    const std::string& dbName,
    const ReadPreferenceSetting& readPref,
    repl::ReadConcernLevel readConcernLevel) {
    dassert(dbName != "admin" && dbName != "config");

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              readPref,
                                              readConcernLevel,
                                              DatabaseType::ConfigNS,
                                              BSON(DatabaseType::name(dbName)),
                                              BSONObj(),
                                              boost::none);
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

    return repl::OpTimeWith<DatabaseType>(parseStatus.getValue(), docsWithOpTime.opTime);
}

StatusWith<repl::OpTimeWith<CollectionType>> ShardingCatalogClientImpl::getCollection(
    OperationContext* opCtx, const NamespaceString& nss, repl::ReadConcernLevel readConcernLevel) {
    auto statusFind = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcernLevel,
                                              CollectionType::ConfigNS,
                                              BSON(CollectionType::fullNs(nss.ns())),
                                              BSONObj(),
                                              1);
    if (!statusFind.isOK()) {
        return statusFind.getStatus();
    }

    const auto& retOpTimePair = statusFind.getValue();
    const auto& retVal = retOpTimePair.value;
    if (retVal.empty()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << nss.ns() << " not found");
    }

    invariant(retVal.size() == 1);

    auto parseStatus = CollectionType::fromBSON(retVal.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    auto collType = parseStatus.getValue();
    if (collType.getDropped()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << nss.ns() << " was dropped");
    }

    return repl::OpTimeWith<CollectionType>(collType, retOpTimePair.opTime);
}

StatusWith<std::vector<CollectionType>> ShardingCatalogClientImpl::getCollections(
    OperationContext* opCtx,
    const std::string* dbName,
    OpTime* opTime,
    repl::ReadConcernLevel readConcernLevel) {
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      string(str::stream() << "^" << pcrecpp::RE::QuoteMeta(*dbName) << "\\."));
    }

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcernLevel,
                                              CollectionType::ConfigNS,
                                              b.obj(),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsOpTimePair = findStatus.getValue();

    std::vector<CollectionType> collections;
    for (const BSONObj& obj : docsOpTimePair.value) {
        const auto collectionResult = CollectionType::fromBSON(obj);
        if (!collectionResult.isOK()) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "error while parsing " << CollectionType::ConfigNS.ns()
                                  << " document: "
                                  << obj
                                  << " : "
                                  << collectionResult.getStatus().toString()};
        }

        collections.push_back(collectionResult.getValue());
    }

    if (opTime) {
        *opTime = docsOpTimePair.opTime;
    }

    return collections;
}

std::vector<NamespaceString> ShardingCatalogClientImpl::getAllShardedCollectionsForDb(
    OperationContext* opCtx, StringData dbName, repl::ReadConcernLevel readConcern) {
    const auto dbNameStr = dbName.toString();

    const std::vector<CollectionType> collectionsOnConfig =
        uassertStatusOK(getCollections(opCtx, &dbNameStr, nullptr, readConcern));

    std::vector<NamespaceString> collectionsToReturn;
    for (const auto& coll : collectionsOnConfig) {
        if (coll.getDropped())
            continue;

        collectionsToReturn.push_back(coll.getNs());
    }

    return collectionsToReturn;
}

StatusWith<BSONObj> ShardingCatalogClientImpl::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              kSettingsNamespace,
                                              BSON("_id" << key),
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

    invariant(docs.size() == 1);
    return docs.front();
}

StatusWith<VersionType> ShardingCatalogClientImpl::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        readConcern,
        VersionType::ConfigNS,
        BSONObj(),
        BSONObj(),
        boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().docs;

    if (queryResults.size() > 1) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS.ns()};
    }

    if (queryResults.empty()) {
        VersionType versionInfo;
        versionInfo.setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setCurrentVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setClusterId(OID{});
        return versionInfo;
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return versionTypeResult.getStatus().withContext(
            str::stream() << "Unable to parse config.version document " << versionDoc);
    }

    auto validationStatus = versionTypeResult.getValue().validate();
    if (!validationStatus.isOK()) {
        return Status(validationStatus.withContext(
            str::stream() << "Unable to validate config.version document " << versionDoc));
    }

    return versionTypeResult.getValue();
}

StatusWith<std::vector<std::string>> ShardingCatalogClientImpl::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardId) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              DatabaseType::ConfigNS,
                                              BSON(DatabaseType::primary(shardId.toString())),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    std::vector<std::string> dbs;
    for (const BSONObj& obj : findStatus.getValue().value) {
        string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            return status;
        }

        dbs.push_back(dbName);
    }

    return dbs;
}

StatusWith<std::vector<ChunkType>> ShardingCatalogClientImpl::getChunks(
    OperationContext* opCtx,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<int> limit,
    OpTime* opTime,
    repl::ReadConcernLevel readConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              readConcern == repl::ReadConcernLevel::kMajorityReadConcern);

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(
        opCtx, kConfigReadSelector, readConcern, ChunkType::ConfigNS, query, sort, longLimit);
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load chunks");
    }

    const auto& chunkDocsOpTimePair = findStatus.getValue();

    std::vector<ChunkType> chunks;
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
        auto chunkRes = ChunkType::fromConfigBSON(obj);
        if (!chunkRes.isOK()) {
            return chunkRes.getStatus().withContext(stream() << "Failed to parse chunk with id "
                                                             << obj[ChunkType::name()]);
        }

        chunks.push_back(chunkRes.getValue());
    }

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return chunks;
}

StatusWith<std::vector<TagsType>> ShardingCatalogClientImpl::getTagsForCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              TagsType::ConfigNS,
                                              BSON(TagsType::ns(nss.ns())),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus().withContext("Failed to load tags");
    }

    const auto& tagDocsOpTimePair = findStatus.getValue();

    std::vector<TagsType> tags;
    for (const BSONObj& obj : tagDocsOpTimePair.value) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            return tagRes.getStatus().withContext(str::stream() << "Failed to parse tag with id "
                                                                << obj[TagsType::tag()]);
        }

        tags.push_back(tagRes.getValue());
    }

    return tags;
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientImpl::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    std::vector<ShardType> shards;
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              ShardType::ConfigNS,
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().value) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            return shardRes.getStatus().withContext(stream() << "Failed to parse shard document "
                                                             << doc);
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return validateStatus.withContext(stream() << "Failed to validate shard document "
                                                       << doc);
        }

        shards.push_back(shardRes.getValue());
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards),
                                                    findStatus.getValue().opTime};
}

bool ShardingCatalogClientImpl::runUserManagementWriteCommand(OperationContext* opCtx,
                                                              const std::string& commandName,
                                                              const std::string& dbname,
                                                              const BSONObj& cmdObj,
                                                              BSONObjBuilder* result) {
    BSONObj cmdToRun = cmdObj;
    {
        // Make sure that if the command has a write concern that it is w:1 or w:majority, and
        // convert w:1 or no write concern to w:majority before sending.
        WriteConcernOptions writeConcern;
        writeConcern.reset();

        BSONElement writeConcernElement = cmdObj[WriteConcernOptions::kWriteConcernField];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            Status status = writeConcern.parse(writeConcernElement.Obj());
            if (!status.isOK()) {
                return CommandHelpers::appendCommandStatusNoThrow(*result, status);
            }

            if (!(writeConcern.wNumNodes == 1 ||
                  writeConcern.wMode == WriteConcernOptions::kMajority)) {
                return CommandHelpers::appendCommandStatusNoThrow(
                    *result,
                    {ErrorCodes::InvalidOptions,
                     str::stream() << "Invalid replication write concern. User management write "
                                      "commands may only use w:1 or w:'majority', got: "
                                   << writeConcern.toBSON()});
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
                if (WriteConcernOptions::kWriteConcernField == e.fieldName()) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto response =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            dbname,
            cmdToRun,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kNotIdempotent);

    if (!response.isOK()) {
        return CommandHelpers::appendCommandStatusNoThrow(*result, response.getStatus());
    }
    if (!response.getValue().commandStatus.isOK()) {
        return CommandHelpers::appendCommandStatusNoThrow(*result,
                                                          response.getValue().commandStatus);
    }
    if (!response.getValue().writeConcernStatus.isOK()) {
        return CommandHelpers::appendCommandStatusNoThrow(*result,
                                                          response.getValue().writeConcernStatus);
    }

    CommandHelpers::filterCommandReplyForPassthrough(response.getValue().response, result);
    return true;
}

bool ShardingCatalogClientImpl::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    auto resultStatus =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigPrimaryPreferredSelector,
            dbname,
            cmdObj,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent);
    if (resultStatus.isOK()) {
        CommandHelpers::filterCommandReplyForPassthrough(resultStatus.getValue().response, result);
        return resultStatus.getValue().commandStatus.isOK();
    }

    return CommandHelpers::appendCommandStatusNoThrow(*result, resultStatus.getStatus());  // XXX
}

Status ShardingCatalogClientImpl::applyChunkOpsDeprecated(OperationContext* opCtx,
                                                          const BSONArray& updateOps,
                                                          const BSONArray& preCondition,
                                                          const NamespaceString& nss,
                                                          const ChunkVersion& lastChunkVersion,
                                                          const WriteConcernOptions& writeConcern,
                                                          repl::ReadConcernLevel readConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              (readConcern == repl::ReadConcernLevel::kMajorityReadConcern &&
               writeConcern.wMode == WriteConcernOptions::kMajority));
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition
                                  << WriteConcernOptions::kWriteConcernField
                                  << writeConcern.toBSON());

    auto response =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "config",
            cmd,
            Shard::RetryPolicy::kIdempotent);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = response.getValue().commandStatus.isOK()
        ? std::move(response.getValue().writeConcernStatus)
        : std::move(response.getValue().commandStatus);

    // TODO (Dianna) This fail point needs to be reexamined when CommitChunkMigration is in:
    // migrations will no longer be able to exercise it, so split or merge will need to do so.
    // SERVER-22659.
    if (MONGO_FAIL_POINT(failApplyChunkOps)) {
        status = Status(ErrorCodes::InternalError, "Failpoint 'failApplyChunkOps' generated error");
    }

    if (!status.isOK()) {
        string errMsg;

        // This could be a blip in the network connectivity. Check if the commit request made it.
        //
        // If all the updates were successfully written to the chunks collection, the last
        // document in the list of updates should be returned from a query to the chunks
        // collection. The last chunk can be identified by namespace and version number.

        warning() << "chunk operation commit failed and metadata will be revalidated"
                  << causedBy(redact(status));

        // Look for the chunk in this shard whose version got bumped. We assume that if that
        // mod made it to the config server, then transaction was successful.
        BSONObjBuilder query;
        lastChunkVersion.addToBSON(query, ChunkType::lastmod());
        query.append(ChunkType::ns(), nss.ns());
        auto chunkWithStatus = getChunks(opCtx, query.obj(), BSONObj(), 1, nullptr, readConcern);

        if (!chunkWithStatus.isOK()) {
            errMsg = str::stream()
                << "getChunks function failed, unable to validate chunk "
                << "operation metadata: " << chunkWithStatus.getStatus().toString()
                << ". applyChunkOpsDeprecated failed to get confirmation "
                << "of commit. Unable to save chunk ops. Command: " << cmd
                << ". Result: " << response.getValue().response;
            return status.withContext(errMsg);
        };

        const auto& newestChunk = chunkWithStatus.getValue();

        if (newestChunk.empty()) {
            errMsg = str::stream() << "chunk operation commit failed: version "
                                   << lastChunkVersion.toString()
                                   << " doesn't exist in namespace: " << nss.ns()
                                   << ". Unable to save chunk ops. Command: " << cmd
                                   << ". Result: " << response.getValue().response;
            return status.withContext(errMsg);
        };

        invariant(newestChunk.size() == 1);
        return Status::OK();
    }

    return Status::OK();
}

DistLockManager* ShardingCatalogClientImpl::getDistLockManager() {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void ShardingCatalogClientImpl::writeConfigServerDirect(OperationContext* opCtx,
                                                        const BatchedCommandRequest& batchRequest,
                                                        BatchedCommandResponse* batchResponse) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    *batchResponse = configShard->runBatchWriteCommand(opCtx,
                                                       Shard::kDefaultConfigCommandTimeout,
                                                       batchRequest,
                                                       Shard::RetryPolicy::kNotIdempotent);
}

Status ShardingCatalogClientImpl::insertConfigDocument(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kAdminDb || nss.db() == NamespaceString::kConfigDb);

    const BSONElement idField = doc.getField("_id");
    invariant(!idField.eoo());

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({doc});
        return insertOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        auto response = configShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kNoRetry);

        Status status = response.toStatus();

        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            // Pretend like the operation is idempotent because we're handling DuplicateKey errors
            // specially
            continue;
        }

        // If we get DuplicateKey error on the first attempt to insert, this definitively means that
        // we are trying to insert the same entry a second time, so error out. If it happens on a
        // retry attempt though, it is not clear whether we are actually inserting a duplicate key
        // or it is because we failed to wait for write concern on the first attempt. In order to
        // differentiate, fetch the entry and check.
        if (retry > 1 && status == ErrorCodes::DuplicateKey) {
            LOG(1) << "Insert retry failed because of duplicate key error, rechecking.";

            auto fetchDuplicate =
                _exhaustiveFindOnConfig(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        repl::ReadConcernLevel::kMajorityReadConcern,
                                        nss,
                                        idField.wrap(),
                                        BSONObj(),
                                        boost::none);
            if (!fetchDuplicate.isOK()) {
                return fetchDuplicate.getStatus();
            }

            auto existingDocs = fetchDuplicate.getValue().value;
            if (existingDocs.empty()) {
                return {ErrorCodes::DuplicateKey,
                        stream() << "DuplicateKey error was returned after a retry attempt, but no "
                                    "documents were found. This means a concurrent change occurred "
                                    "together with the retries. Original error was "
                                 << status.toString()};
            }

            invariant(existingDocs.size() == 1);

            BSONObj existing = std::move(existingDocs.front());
            if (existing.woCompare(doc) == 0) {
                // Documents match, so treat the operation as success
                return Status::OK();
            }
        }

        return status;
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return _updateConfigDocument(opCtx, nss, query, update, upsert, writeConcern);
}

StatusWith<bool> ShardingCatalogClientImpl::_updateConfigDocument(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    const BSONElement idField = query.getField("_id");
    invariant(!idField.eoo());

    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(update);
            entry.setUpsert(upsert);
            entry.setMulti(false);
            return entry;
        }()});
        return updateOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);

    Status status = response.toStatus();
    if (!status.isOK()) {
        return status;
    }

    const auto nSelected = response.getN();
    invariant(nSelected == 0 || nSelected == 1);
    return (nSelected == 1);
}

Status ShardingCatalogClientImpl::removeConfigDocuments(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
    return response.toStatus();
}

Status ShardingCatalogClientImpl::_createCappedConfigCollection(
    OperationContext* opCtx,
    StringData collName,
    int cappedSize,
    const WriteConcernOptions& writeConcern) {
    BSONObj createCmd = BSON("create" << collName << "capped" << true << "size" << cappedSize
                                      << WriteConcernOptions::kWriteConcernField
                                      << writeConcern.toBSON());

    auto result =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "config",
            createCmd,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent);

    if (!result.isOK()) {
        return result.getStatus();
    }

    if (!result.getValue().commandStatus.isOK()) {
        if (result.getValue().commandStatus == ErrorCodes::NamespaceExists) {
            if (result.getValue().writeConcernStatus.isOK()) {
                return Status::OK();
            } else {
                return result.getValue().writeConcernStatus;
            }
        } else {
            return result.getValue().commandStatus;
        }
    }

    return result.getValue().writeConcernStatus;
}

StatusWith<repl::OpTimeWith<vector<BSONObj>>> ShardingCatalogClientImpl::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcern,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto response = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx, readPref, readConcern, nss, query, sort, limit);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return repl::OpTimeWith<vector<BSONObj>>(std::move(response.getValue().docs),
                                             response.getValue().opTime);
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientImpl::getNewKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus = config->exhaustiveFindOnConfig(opCtx,
                                                     kConfigReadSelector,
                                                     readConcernLevel,
                                                     KeysCollectionDocument::ConfigNS,
                                                     queryBuilder.obj(),
                                                     BSON("expiresAt" << 1),
                                                     boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeysCollectionDocument> keys;
    for (auto&& keyDoc : keyDocs) {
        auto parseStatus = KeysCollectionDocument::fromBSON(keyDoc);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        keys.push_back(std::move(parseStatus.getValue()));
    }

    return keys;
}

}  // namespace mongo
