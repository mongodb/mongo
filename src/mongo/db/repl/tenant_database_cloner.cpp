/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/tenant_collection_cloner.h"
#include "mongo/db/repl/tenant_database_cloner.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {
namespace repl {

// Failpoint which the tenant database cloner to hang after it has successully run listCollections
// and recorded the results and the operationTime.
MONGO_FAIL_POINT_DEFINE(tenantDatabaseClonerHangAfterGettingOperationTime);
// Failpoint to skip comparing the list of collections that are already cloned, instead it will
// resume the cloning from the beginning of the list, that's provided by
// TenantDatabaseCloner::listCollectionsStage.
MONGO_FAIL_POINT_DEFINE(skiplistExistingCollectionsStage);

TenantDatabaseCloner::TenantDatabaseCloner(const std::string& dbName,
                                           TenantMigrationSharedData* sharedData,
                                           const HostAndPort& source,
                                           DBClientConnection* client,
                                           StorageInterface* storageInterface,
                                           ThreadPool* dbPool,
                                           StringData tenantId)
    : TenantBaseCloner(
          "TenantDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _dbName(dbName),
      _listCollectionsStage("listCollections", this, &TenantDatabaseCloner::listCollectionsStage),
      _listExistingCollectionsStage(
          "listExistingCollections", this, &TenantDatabaseCloner::listExistingCollectionsStage),
      _tenantId(tenantId) {
    invariant(!dbName.empty());
    _stats.dbname = dbName;
}

BaseCloner::ClonerStages TenantDatabaseCloner::getStages() {
    return {&_listCollectionsStage, &_listExistingCollectionsStage};
}

void TenantDatabaseCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
}

BaseCloner::AfterStageBehavior TenantDatabaseCloner::listCollectionsStage() {
    // This will be set after a successful listCollections command.
    _operationTime = Timestamp();

    auto collectionInfos = getClient()->getCollectionInfos(
        DatabaseName(boost::none, _dbName), ListCollectionsFilter::makeTypeCollectionFilter());

    // Do a majority read on the sync source to make sure the collections listed exist on a majority
    // of nodes in the set. We do not check the rollbackId - rollback would lead to the sync source
    // closing connections so the stage would fail.
    _operationTime = getClient()->getOperationTime();

    tenantDatabaseClonerHangAfterGettingOperationTime.executeIf(
        [&](const BSONObj&) {
            while (MONGO_unlikely(tenantDatabaseClonerHangAfterGettingOperationTime.shouldFail()) &&
                   !mustExit()) {
                LOGV2(4881605,
                      "tenantDatabaseClonerHangAfterGettingOperationTime fail point "
                      "enabled. Blocking until fail point is disabled",
                      "dbName"_attr = _dbName,
                      "tenantId"_attr = _tenantId);
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            // Only hang when cloning the specified collection, or if no collection was specified.
            auto dbName = data["dbName"].str();
            return dbName.empty() || dbName == _dbName;
        });

    BSONObj readResult;
    BSONObj cmd = ClonerUtils::buildMajorityWaitRequest(_operationTime);
    getClient()->runCommand(
        DatabaseName(boost::none, "admin"), cmd, readResult, QueryOption_SecondaryOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "TenantDatabaseCloner failed to get listCollections result majority-committed");

    {
        // _operationTime is now majority committed on donor.
        //
        // Tenant Migration recipient oplog fetcher doesn't care about the donor term field in
        // TenantMigrationRecipientDocument::DataConsistentStopDonorOpTime, which is determined by
        // TenantMigrationSharedData::_lastVisibleOpTime. So, it's ok to build a fake OpTime with
        // term set as OpTime::kUninitializedTerm.
        stdx::lock_guard<TenantMigrationSharedData> lk(*getSharedData());
        getSharedData()->setLastVisibleOpTime(lk,
                                              OpTime(_operationTime, OpTime::kUninitializedTerm));
    }

    // Process and verify the listCollections results.
    stdx::unordered_set<std::string> seen;
    for (auto&& info : collectionInfos) {
        ListCollectionResult result;
        try {
            result = ListCollectionResult::parse(
                IDLParserContext("TenantDatabaseCloner::listCollectionsStage"), info);
        } catch (const DBException& e) {
            uasserted(
                ErrorCodes::FailedToParse,
                e.toStatus()
                    .withContext(str::stream() << "Collection info could not be parsed : " << info)
                    .reason());
        }
        NamespaceString collectionNamespace(_dbName, result.getName());
        if (collectionNamespace.isSystem() && !collectionNamespace.isReplicated()) {
            LOGV2_DEBUG(4881602,
                        1,
                        "Database cloner skipping 'system' collection",
                        logAttrs(collectionNamespace),
                        "tenantId"_attr = _tenantId);
            continue;
        }
        LOGV2_DEBUG(4881603,
                    2,
                    "Allowing cloning of collectionInfo",
                    "info"_attr = info,
                    "db"_attr = _dbName,
                    "tenantId"_attr = _tenantId);

        bool isDuplicate = seen.insert(result.getName().toString()).second;
        uassert(4881604,
                str::stream() << "collection info contains duplicate collection name "
                              << "'" << result.getName() << "': " << info,
                isDuplicate);

        // While UUID is a member of CollectionOptions, listCollections does not return the
        // collectionUUID there as part of the options, but instead places it in the 'info'
        // field.
        // We need to move it back to CollectionOptions to create the collection properly.
        result.getOptions().uuid = result.getInfo().getUuid();
        _collections.emplace_back(collectionNamespace, result.getOptions());
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantDatabaseCloner::listExistingCollectionsStage() {
    if (MONGO_unlikely(skiplistExistingCollectionsStage.shouldFail())) {
        LOGV2(6312900,
              "skiplistExistingCollectionsStage failpoint is enabled. "
              "Tenant DatabaseCloner resumes cloning",
              "migrationId"_attr = getSharedData()->getMigrationId(),
              "tenantId"_attr = _tenantId,
              "resumeFrom"_attr = _collections.front().first);
        return kContinueNormally;
    }
    auto opCtx = cc().makeOperationContext();
    DBDirectClient client(opCtx.get());
    tenantMigrationInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationInfo>(getSharedData()->getMigrationId());

    long long sizeOfCurrCollOnDisk = 0;
    long long approxTotalDBSizeOnDisk = 0;

    std::vector<UUID> clonedCollectionUUIDs;
    auto collectionInfos = client.getCollectionInfos(
        DatabaseName(boost::none, _dbName), ListCollectionsFilter::makeTypeCollectionFilter());
    for (auto&& info : collectionInfos) {
        ListCollectionResult result;
        try {
            result = ListCollectionResult::parse(
                IDLParserContext("TenantDatabaseCloner::listExistingCollectionsStage"), info);
        } catch (const DBException& e) {
            uasserted(
                ErrorCodes::FailedToParse,
                e.toStatus()
                    .withContext(str::stream() << "Collection info could not be parsed : " << info)
                    .reason());
        }
        NamespaceString collectionNamespace(_dbName, result.getName());
        if (collectionNamespace.isSystem() && !collectionNamespace.isReplicated()) {
            LOGV2_DEBUG(5271600,
                        1,
                        "Tenant database cloner skipping 'system' collection",
                        "migrationId"_attr = getSharedData()->getMigrationId(),
                        "tenantId"_attr = _tenantId,
                        logAttrs(collectionNamespace));
            continue;
        }
        clonedCollectionUUIDs.emplace_back(result.getInfo().getUuid());

        BSONObj res;
        client.runCommand(
            DatabaseName(boost::none, _dbName), BSON("collStats" << result.getName()), res);
        if (auto status = getStatusFromCommandResult(res); !status.isOK()) {
            LOGV2_WARNING(5522901,
                          "Skipping recording of data size metrics for database due to failure "
                          "in the 'collStats' command, tenant migration stats may be inaccurate.",
                          "db"_attr = _dbName,
                          "coll"_attr = result.getName(),
                          "migrationId"_attr = getSharedData()->getMigrationId(),
                          "tenantId"_attr = _tenantId,
                          "status"_attr = status);
        } else {
            sizeOfCurrCollOnDisk = res.getField("size").safeNumberLong();
            approxTotalDBSizeOnDisk += sizeOfCurrCollOnDisk;
        }
    }

    if (getSharedData()->getResumePhase() == ResumePhase::kNone) {
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Tenant '" << _tenantId
                              << "': collections already exist prior to data sync",
                clonedCollectionUUIDs.empty());
        return kContinueNormally;
    }

    // We are resuming, restart from the collection whose UUID compared greater than or equal to
    // the last collection we have on disk.
    if (!clonedCollectionUUIDs.empty()) {
        const auto& lastClonedCollectionUUID = clonedCollectionUUIDs.back();
        const auto& startingCollection = std::lower_bound(
            _collections.begin(),
            _collections.end(),
            lastClonedCollectionUUID,
            [](const auto& collection, const auto& uuid) { return collection.second.uuid < uuid; });
        {
            stdx::lock_guard<Latch> lk(_mutex);
            if (startingCollection != _collections.end() &&
                startingCollection->second.uuid == lastClonedCollectionUUID) {
                _stats.clonedCollectionsBeforeFailover = clonedCollectionUUIDs.size() - 1;

                // When the last collection is partially cloned, we exclude it from the total size
                // on disk, as the partially cloned collections stats will be added by the cloner
                // on demand.
                _stats.approxTotalBytesCopied = approxTotalDBSizeOnDisk - sizeOfCurrCollOnDisk;
            } else {
                _stats.clonedCollectionsBeforeFailover = clonedCollectionUUIDs.size();
                _stats.approxTotalBytesCopied = approxTotalDBSizeOnDisk;
            }
        }
        _collections.erase(_collections.begin(), startingCollection);
        if (!_collections.empty()) {
            LOGV2(5271601,
                  "Tenant DatabaseCloner resumes cloning",
                  "migrationId"_attr = getSharedData()->getMigrationId(),
                  "tenantId"_attr = _tenantId,
                  "resumeFrom"_attr = _collections.front().first);
        } else {
            LOGV2(5271602,
                  "Tenant DatabaseCloner has already cloned all collections",
                  "migrationId"_attr = getSharedData()->getMigrationId(),
                  "tenantId"_attr = _tenantId,
                  "dbName"_attr = _dbName);
        }
    }

    return kContinueNormally;
}

bool TenantDatabaseCloner::isMyFailPoint(const BSONObj& data) const {
    return data["database"].str() == _dbName && BaseCloner::isMyFailPoint(data);
}

void TenantDatabaseCloner::postStage() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.collections = _collections.size();
        _stats.collectionStats.reserve(_collections.size());
        for (const auto& coll : _collections) {
            _stats.collectionStats.emplace_back();
            _stats.collectionStats.back().ns = coll.first.ns();
        }
    }
    for (const auto& coll : _collections) {
        auto& sourceNss = coll.first;
        auto& collectionOptions = coll.second;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _currentCollectionCloner =
                std::make_unique<TenantCollectionCloner>(sourceNss,
                                                         collectionOptions,
                                                         getSharedData(),
                                                         getSource(),
                                                         getClient(),
                                                         getStorageInterface(),
                                                         getDBPool(),
                                                         _tenantId);
        }
        auto collStatus = _currentCollectionCloner->run();
        if (collStatus.isOK()) {
            LOGV2_DEBUG(4881600,
                        1,
                        "Tenant collection clone finished",
                        logAttrs(sourceNss),
                        "tenantId"_attr = _tenantId);
        } else {
            LOGV2_ERROR(4881601,
                        "Tenant collection clone failed",
                        logAttrs(sourceNss),
                        "error"_attr = collStatus.toString(),
                        "tenantId"_attr = _tenantId);
            auto message = collStatus.withContext(str::stream() << "Error cloning collection '"
                                                                << sourceNss.toString() << "'");
            setSyncFailedStatus(collStatus.withReason(message.toString()));
        }
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _stats.collectionStats[_stats.clonedCollections] = _currentCollectionCloner->getStats();
            _stats.approxTotalBytesCopied +=
                _stats.collectionStats[_stats.clonedCollections].approxTotalBytesCopied;
            _currentCollectionCloner = nullptr;
            // Abort the tenant database cloner if the collection clone failed.
            if (!collStatus.isOK())
                return;
            _stats.clonedCollections++;
        }
    }
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.end = getSharedData()->getClock()->now();
}

TenantDatabaseCloner::Stats TenantDatabaseCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    TenantDatabaseCloner::Stats stats = _stats;
    if (_currentCollectionCloner) {
        stats.collectionStats[_stats.clonedCollections] = _currentCollectionCloner->getStats();
        stats.approxTotalBytesCopied +=
            stats.collectionStats[stats.clonedCollections].approxTotalBytesCopied;
    }
    return stats;
}

std::string TenantDatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj TenantDatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("dbname", dbname);
    append(&bob);
    return bob.obj();
}

void TenantDatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("clonedCollectionsBeforeFailover",
                          static_cast<long long>(clonedCollectionsBeforeFailover));
    builder->appendNumber("collections", static_cast<long long>(collections));
    builder->appendNumber("clonedCollections", static_cast<long long>(clonedCollections));
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }

    for (auto&& collection : collectionStats) {
        BSONObjBuilder collectionBuilder(builder->subobjStart(collection.ns));
        collection.append(&collectionBuilder);
        collectionBuilder.doneFast();
    }
}

Timestamp TenantDatabaseCloner::getOperationTime_forTest() {
    return _operationTime;
}

}  // namespace repl
}  // namespace mongo
