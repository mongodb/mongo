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

#include <algorithm>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/tenant_all_database_cloner.h"
#include "mongo/db/repl/tenant_database_cloner.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {
namespace repl {

// Failpoint which the tenant database cloner to hang after it has successully run listDatabases
// and recorded the results and the operationTime.
MONGO_FAIL_POINT_DEFINE(tenantAllDatabaseClonerHangAfterGettingOperationTime);

TenantAllDatabaseCloner::TenantAllDatabaseCloner(TenantMigrationSharedData* sharedData,
                                                 const HostAndPort& source,
                                                 DBClientConnection* client,
                                                 StorageInterface* storageInterface,
                                                 ThreadPool* dbPool,
                                                 StringData tenantId)
    : TenantBaseCloner(
          "TenantAllDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _tenantId(tenantId),
      _listDatabasesStage("listDatabases", this, &TenantAllDatabaseCloner::listDatabasesStage),
      _listExistingDatabasesStage(
          "listExistingDatabases", this, &TenantAllDatabaseCloner::listExistingDatabasesStage),
      _initializeStatsStage(
          "initializeStatsStage", this, &TenantAllDatabaseCloner::initializeStatsStage) {}

BaseCloner::ClonerStages TenantAllDatabaseCloner::getStages() {
    return {&_listDatabasesStage, &_listExistingDatabasesStage, &_initializeStatsStage};
}

void TenantAllDatabaseCloner::preStage() {
    stdx::lock_guard lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
}

BaseCloner::AfterStageBehavior TenantAllDatabaseCloner::listDatabasesStage() {
    // This will be set after a successful listDatabases command.
    _operationTime = Timestamp();

    BSONObj res;
    const BSONObj filter = ClonerUtils::makeTenantDatabaseFilter(_tenantId);
    auto databasesArray = getClient()->getDatabaseInfos(filter, true /* nameOnly */);

    // Do a majority read on the sync source to make sure the databases listed exist on a majority
    // of nodes in the set. We do not check the rollbackId - rollback would lead to the sync source
    // closing connections so the stage would fail.
    _operationTime = getClient()->getOperationTime();

    if (MONGO_unlikely(tenantAllDatabaseClonerHangAfterGettingOperationTime.shouldFail())) {
        LOGV2(4881504,
              "Failpoint 'tenantAllDatabaseClonerHangAfterGettingOperationTime' enabled. Blocking "
              "until it is disabled.",
              "tenantId"_attr = _tenantId);
        tenantAllDatabaseClonerHangAfterGettingOperationTime.pauseWhileSet();
    }

    BSONObj readResult;
    BSONObj cmd = ClonerUtils::buildMajorityWaitRequest(_operationTime);
    getClient()->runCommand(
        DatabaseName(boost::none, "admin"), cmd, readResult, QueryOption_SecondaryOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "TenantAllDatabaseCloner failed to get listDatabases result majority-committed");

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

    // Process and verify the listDatabases results.
    for (const auto& dbBSON : databasesArray) {
        LOGV2_DEBUG(4881508,
                    2,
                    "Cloner received listDatabases entry",
                    "db"_attr = dbBSON,
                    "tenantId"_attr = _tenantId);
        uassert(4881505, "Result from donor must have 'name' set", dbBSON.hasField("name"));

        const auto& dbName = dbBSON["name"].str();
        _databases.emplace_back(dbName);
    }

    std::sort(_databases.begin(), _databases.end());
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantAllDatabaseCloner::listExistingDatabasesStage() {
    auto opCtx = cc().makeOperationContext();
    DBDirectClient client(opCtx.get());
    tenantMigrationInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationInfo>(getSharedData()->getMigrationId());

    const BSONObj filter = ClonerUtils::makeTenantDatabaseFilter(_tenantId);
    auto databasesArray = client.getDatabaseInfos(filter, true /* nameOnly */);

    long long approxTotalSizeOnDisk = 0;
    // Use a map to figure out the size of the partially cloned database.
    StringMap<long long> dbNameToSize;

    std::vector<std::string> clonedDatabases;
    for (const auto& dbBSON : databasesArray) {
        LOGV2_DEBUG(5271500,
                    2,
                    "listExistingDatabases entry",
                    "migrationId"_attr = getSharedData()->getMigrationId(),
                    "tenantId"_attr = _tenantId,
                    "db"_attr = dbBSON);
        uassert(5271501,
                "Cloned database from recipient must have 'name' set",
                dbBSON.hasField("name"));

        const auto& dbName = dbBSON["name"].str();
        clonedDatabases.emplace_back(dbName);

        BSONObj res;
        client.runCommand(DatabaseName(boost::none, dbName), BSON("dbStats" << 1), res);
        if (auto status = getStatusFromCommandResult(res); !status.isOK()) {
            LOGV2_WARNING(5522900,
                          "Skipping recording of data size metrics for database due to failure "
                          "in the 'dbStats' command, tenant migration stats may be inaccurate.",
                          "db"_attr = dbName,
                          "migrationId"_attr = getSharedData()->getMigrationId(),
                          "tenantId"_attr = _tenantId,
                          "status"_attr = status);
        } else {
            dbNameToSize[dbName] = res.getField("dataSize").safeNumberLong();
            approxTotalSizeOnDisk += dbNameToSize[dbName];
        }
    }

    if (getSharedData()->getResumePhase() == ResumePhase::kNone) {
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Tenant '" << _tenantId
                              << "': databases already exist prior to data sync",
                clonedDatabases.empty());
        return kContinueNormally;
    }

    // We are resuming, restart from the database alphabetically compared greater than or equal to
    // the last database we have on disk.
    std::sort(clonedDatabases.begin(), clonedDatabases.end());
    if (!clonedDatabases.empty()) {
        const auto& lastClonedDb = clonedDatabases.back();
        const auto& startingDb =
            std::lower_bound(_databases.begin(), _databases.end(), lastClonedDb);
        {
            stdx::lock_guard<Latch> lk(_mutex);
            if (startingDb != _databases.end() && *startingDb == lastClonedDb) {
                _stats.databasesClonedBeforeFailover = clonedDatabases.size() - 1;

                // When the 'startingDb' matches the 'lastClonedDb', the 'startingDb' is currently
                // partially cloned. Therefore, exclude the 'startingDb' when calculating the size,
                // as it is counted on demand by the database cloner.
                _stats.approxTotalBytesCopied =
                    approxTotalSizeOnDisk - dbNameToSize.at(*startingDb);
            } else {
                _stats.databasesClonedBeforeFailover = clonedDatabases.size();
                _stats.approxTotalBytesCopied = approxTotalSizeOnDisk;
            }
        }
        _databases.erase(_databases.begin(), startingDb);
        if (!_databases.empty()) {
            LOGV2(5271502,
                  "Tenant AllDatabaseCloner resumes cloning",
                  "migrationId"_attr = getSharedData()->getMigrationId(),
                  "tenantId"_attr = _tenantId,
                  "resumeFrom"_attr = _databases.front());
        } else {
            LOGV2(5271503,
                  "Tenant AllDatabaseCloner has already cloned all databases",
                  "migrationId"_attr = getSharedData()->getMigrationId(),
                  "tenantId"_attr = _tenantId);
        }
    }

    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantAllDatabaseCloner::initializeStatsStage() {
    // Finish calculating the size of the databases that were either partially cloned or
    // completely un-cloned from a previous migration. Perform this before grabbing the _mutex,
    // as commands are being sent over the network.
    long long approxTotalDataSizeLeftOnRemote = 0;
    for (const auto& dbName : _databases) {
        BSONObj res;
        getClient()->runCommand(DatabaseName(boost::none, dbName), BSON("dbStats" << 1), res);
        if (auto status = getStatusFromCommandResult(res); !status.isOK()) {
            LOGV2_WARNING(5426600,
                          "Skipping recording of data size metrics for database due to failure "
                          "in the 'dbStats' command, tenant migration stats may be inaccurate.",
                          "db"_attr = dbName,
                          "migrationId"_attr = getSharedData()->getMigrationId(),
                          "tenantId"_attr = _tenantId,
                          "status"_attr = status);
        } else {
            approxTotalDataSizeLeftOnRemote += res.getField("dataSize").safeNumberLong();
        }
    }

    stdx::lock_guard<Latch> lk(_mutex);
    // The 'approxTotalDataSize' is the sum of the size copied so far and the size left to be
    // copied.
    _stats.approxTotalDataSize = _stats.approxTotalBytesCopied + approxTotalDataSizeLeftOnRemote;
    _stats.databasesCloned = 0;
    _stats.databasesToClone = _databases.size();
    _stats.databaseStats.reserve(_databases.size());
    for (const auto& dbName : _databases) {
        _stats.databaseStats.emplace_back();
        _stats.databaseStats.back().dbname = dbName;
    }

    return kContinueNormally;
}

void TenantAllDatabaseCloner::postStage() {
    for (const auto& dbName : _databases) {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _currentDatabaseCloner = std::make_unique<TenantDatabaseCloner>(dbName,
                                                                            getSharedData(),
                                                                            getSource(),
                                                                            getClient(),
                                                                            getStorageInterface(),
                                                                            getDBPool(),
                                                                            _tenantId);
        }
        auto dbStatus = _currentDatabaseCloner->run();
        if (dbStatus.isOK()) {
            LOGV2_DEBUG(4881500,
                        1,
                        "Tenant migration database clone finished",
                        "dbName"_attr = dbName,
                        "status"_attr = dbStatus,
                        "tenantId"_attr = _tenantId);
        } else {
            LOGV2_WARNING(4881501,
                          "Tenant migration database clone failed",
                          "dbName"_attr = dbName,
                          "dbNumber"_attr = (_stats.databasesCloned + 1),
                          "totalDbs"_attr = _databases.size(),
                          "error"_attr = dbStatus.toString(),
                          "tenantId"_attr = _tenantId);
            setSyncFailedStatus(dbStatus);
            return;
        }
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
            _stats.approxTotalBytesCopied +=
                _stats.databaseStats[_stats.databasesCloned].approxTotalBytesCopied;
            _currentDatabaseCloner = nullptr;
            _stats.databasesCloned++;
        }
    }
}

TenantAllDatabaseCloner::Stats TenantAllDatabaseCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    TenantAllDatabaseCloner::Stats stats = _stats;
    if (_currentDatabaseCloner) {
        stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
        stats.approxTotalBytesCopied +=
            stats.databaseStats[stats.databasesCloned].approxTotalBytesCopied;
    }
    return stats;
}

std::string TenantAllDatabaseCloner::toString() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return str::stream() << "tenant migration --"
                         << " active:" << isActive(lk) << " status:" << getStatus(lk).toString()
                         << " source:" << getSource() << " tenantId: " << _tenantId
                         << " db cloners completed:" << _stats.databasesCloned;
}

std::string TenantAllDatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj TenantAllDatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void TenantAllDatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("databasesClonedBeforeFailover",
                          static_cast<long long>(databasesClonedBeforeFailover));
    builder->appendNumber("databasesToClone", static_cast<long long>(databasesToClone));
    builder->appendNumber("databasesCloned", static_cast<long long>(databasesCloned));
    builder->appendNumber("approxTotalDataSize", approxTotalDataSize);
    builder->appendNumber("approxTotalBytesCopied", approxTotalBytesCopied);
    for (auto&& db : databaseStats) {
        BSONObjBuilder dbBuilder(builder->subobjStart(db.dbname));
        db.append(&dbBuilder);
        dbBuilder.doneFast();
    }
}

Timestamp TenantAllDatabaseCloner::getOperationTime_forTest() {
    return _operationTime;
}

}  // namespace repl
}  // namespace mongo
