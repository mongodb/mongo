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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/tenant_all_database_cloner.h"
#include "mongo/db/repl/tenant_database_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

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
      _listDatabasesStage("listDatabases", this, &TenantAllDatabaseCloner::listDatabasesStage) {}

BaseCloner::ClonerStages TenantAllDatabaseCloner::getStages() {
    return {&_listDatabasesStage};
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
    getClient()->runCommand("admin", cmd, readResult, QueryOption_SecondaryOk);
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

void TenantAllDatabaseCloner::postStage() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.databasesCloned = 0;
        _stats.databaseStats.reserve(_databases.size());
        for (const auto& dbName : _databases) {
            _stats.databaseStats.emplace_back();
            _stats.databaseStats.back().dbname = dbName;
        }
    }
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
    builder->appendNumber("databasesCloned", databasesCloned);
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
