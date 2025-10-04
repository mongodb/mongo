/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/initial_sync/all_database_cloner.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/str.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync

namespace mongo {
namespace repl {

AllDatabaseCloner::AllDatabaseCloner(InitialSyncSharedData* sharedData,
                                     const HostAndPort& source,
                                     DBClientConnection* client,
                                     StorageInterface* storageInterface,
                                     ThreadPool* dbPool)
    : InitialSyncBaseCloner(
          "AllDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _connectStage("connect", this, &AllDatabaseCloner::connectStage),
      _getInitialSyncIdStage("getInitialSyncId", this, &AllDatabaseCloner::getInitialSyncIdStage),
      _listDatabasesStage("listDatabases", this, &AllDatabaseCloner::listDatabasesStage) {}

BaseCloner::ClonerStages AllDatabaseCloner::getStages() {
    return {&_connectStage, &_getInitialSyncIdStage, &_listDatabasesStage};
}

Status AllDatabaseCloner::ensurePrimaryOrSecondary(
    const executor::RemoteCommandResponse& helloReply) {
    if (!helloReply.isOK()) {
        LOGV2(21054, "Cannot reconnect because 'hello' command failed");
        return helloReply.status;
    }
    if (helloReply.data["isWritablePrimary"].trueValue() ||
        helloReply.data["secondary"].trueValue())
        return Status::OK();

    // There is a window during startup where a node has an invalid configuration and will have
    // an "hello" response the same as a removed node.  So we must check to see if the node is
    // removed by checking local configuration.
    auto memberData = ReplicationCoordinator::get(getGlobalServiceContext())->getMemberData();
    auto syncSourceIter = std::find_if(
        memberData.begin(), memberData.end(), [source = getSource()](const MemberData& member) {
            return member.getHostAndPort() == source;
        });
    if (syncSourceIter == memberData.end()) {
        Status status(ErrorCodes::NotPrimaryOrSecondary,
                      str::stream() << "Sync source " << getSource()
                                    << " has been removed from the replication configuration.");
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        // Setting the status in the shared data will cancel the initial sync.
        getSharedData()->setStatusIfOK(lk, status);
        return status;
    }

    // We also check if the sync source has gone into initial sync itself.  If so, we'll never be
    // able to sync from it and we should abort the attempt.  Because there is a window during
    // startup where a node will report being in STARTUP2 even if it is not in initial sync,
    // we also check to see if it has a sync source.  A node in STARTUP2 will not have a sync
    // source unless it is in initial sync.
    if (syncSourceIter->getState().startup2() && !syncSourceIter->getSyncSource().empty()) {
        Status status(ErrorCodes::NotPrimaryOrSecondary,
                      str::stream() << "Sync source " << getSource() << " has been resynced.");
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        // Setting the status in the shared data will cancel the initial sync.
        getSharedData()->setStatusIfOK(lk, status);
        return status;
    }
    return Status(ErrorCodes::NotPrimaryOrSecondary,
                  str::stream() << "Cannot connect because sync source " << getSource()
                                << " is neither primary nor secondary.");
}

BaseCloner::AfterStageBehavior AllDatabaseCloner::connectStage() {
    auto* client = getClient();
    // If the client already has the address (from a previous attempt), we must allow it to
    // handle the reconnect itself. This is necessary to get correct backoff behavior.
    if (client->getServerHostAndPort() != getSource()) {
        client->setHandshakeValidationHook(
            [this](const executor::RemoteCommandResponse& helloReply) {
                return ensurePrimaryOrSecondary(helloReply);
            });
        client->connect(getSource(), StringData(), boost::none);
    } else {
        client->ensureConnection();
    }
    uassertStatusOK(replAuthenticate(client).withContext(
        str::stream() << "Failed to authenticate to " << getSource()));
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior AllDatabaseCloner::getInitialSyncIdStage() {
    auto initialSyncId =
        getClient()->findOne(NamespaceString::kDefaultInitialSyncIdNamespace, BSONObj{});
    uassert(ErrorCodes::InitialSyncFailure,
            "Cannot retrieve sync source initial sync ID",
            !initialSyncId.isEmpty());
    InitialSyncIdDocument initialSyncIdDoc =
        InitialSyncIdDocument::parse(initialSyncId, IDLParserContext("initialSyncId"));
    {
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        getSharedData()->setInitialSyncSourceId(lk, initialSyncIdDoc.get_id());
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior AllDatabaseCloner::listDatabasesStage() {
    std::vector<mongo::BSONObj> databasesArray;
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool multiTenancyAndRequireTenantIdEnabled = gMultitenancySupport &&
        fcvSnapshot.isVersionInitialized() && gFeatureFlagRequireTenantID.isEnabled(fcvSnapshot);

    databasesArray = getClient()->getDatabaseInfos(
        BSONObj(),
        true /* nameOnly */,
        false /*authorizedDatabases*/,
        multiTenancyAndRequireTenantIdEnabled /*useListDatabsesForAllTenants*/);

    size_t idxToInsertNextAdmin = 0;
    for (const auto& dbBSON : databasesArray) {
        if (!dbBSON.hasField("name")) {
            LOGV2_DEBUG(21055,
                        1,
                        "Excluding database due to the 'listDatabases' response not containing a "
                        "'name' field for this entry",
                        "db"_attr = dbBSON);
            continue;
        }

        boost::optional<TenantId> tenantId = dbBSON.hasField("tenantId")
            ? boost::make_optional<TenantId>(TenantId::parseFromBSON(dbBSON["tenantId"]))
            : boost::none;
        const DatabaseName dbName = DatabaseNameUtil::deserialize(
            tenantId, dbBSON["name"].str(), SerializationContext::stateDefault());

        if (dbName.isLocalDB()) {
            LOGV2_DEBUG(21056,
                        1,
                        "Excluding database from the 'listDatabases' response",
                        "db"_attr = dbBSON);
            continue;
        } else {
            _databases.emplace_back(dbName);

            // Put admin dbs in the front of the vector.
            if (dbName.isAdminDB()) {
                if (_databases.size() > 1) {
                    std::iter_swap(_databases.begin() + idxToInsertNextAdmin, _databases.end() - 1);
                }
                // Index to track where the last admin db is in the list. If the first db returned
                // is an admin db, we still must bump this.
                idxToInsertNextAdmin++;
            }
        }
    }

    // Ensure the global admin comes first. We inserted all admin dbs at the front of '_databases',
    // find the global admin and move it to the front.
    for (auto i = _databases.begin(); size_t(i - _databases.begin()) != idxToInsertNextAdmin; ++i) {
        if (!(*i).tenantId()) {
            std::iter_swap(_databases.begin(), i);
            break;
        }
    }


    return kContinueNormally;
}

void AllDatabaseCloner::postStage() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _stats.databasesCloned = 0;
        _stats.databasesToClone = _databases.size();
        _stats.databaseStats.reserve(_databases.size());
        for (const auto& dbName : _databases) {
            _stats.databaseStats.emplace_back();
            _stats.databaseStats.back().dbname = dbName;

            BSONObj cmdObj = BSON("dbStats" << 1);
            BSONObjBuilder b(cmdObj);

            BSONObj res;
            getClient()->runCommand(dbName, b.obj(), res);

            // It is possible for the call to 'dbStats' to fail if the sync source contains invalid
            // views. We should not fail initial sync in this case due to the situation where the
            // replica set may have lost majority availability and therefore have no access to a
            // primary to fix the view definitions. Instead, we simply skip recording the data size
            // metrics.
            if (auto status = getStatusFromCommandResult(res); status.isOK()) {
                _stats.dataSize += res.getField("dataSize").safeNumberLong();
            } else {
                LOGV2_DEBUG(4786301,
                            1,
                            "Skipping the recording of initial sync data size metrics due "
                            "to failure in the 'dbStats' command",
                            logAttrs(dbName),
                            "status"_attr = status);
            }
        }
    }

    for (const auto& dbName : _databases) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _currentDatabaseCloner = std::make_unique<DatabaseCloner>(dbName,
                                                                      getSharedData(),
                                                                      getSource(),
                                                                      getClient(),
                                                                      getStorageInterface(),
                                                                      getDBPool());
        }
        auto dbStatus = _currentDatabaseCloner->run();
        if (dbStatus.isOK()) {
            LOGV2_DEBUG(21057,
                        1,
                        "Database clone finished",
                        "dbName"_attr = dbName,
                        "status"_attr = dbStatus);
        } else {
            LOGV2_WARNING(21060,
                          "Database clone failed",
                          "dbName"_attr = dbName,
                          "dbNumber"_attr = (_stats.databasesCloned + 1),
                          "totalDbs"_attr = _databases.size(),
                          "error"_attr = dbStatus.toString());
            setSyncFailedStatus(dbStatus);
            return;
        }
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
            _currentDatabaseCloner = nullptr;
            _stats.databasesCloned++;
            _stats.databasesToClone--;
        }
    }
}

AllDatabaseCloner::Stats AllDatabaseCloner::getStats() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    AllDatabaseCloner::Stats stats = _stats;
    if (_currentDatabaseCloner) {
        stats.databaseStats[_stats.databasesCloned] = _currentDatabaseCloner->getStats();
    }
    return stats;
}

std::string AllDatabaseCloner::toString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return str::stream() << "initial sync --"
                         << " active:" << isActive(lk) << " status:" << getStatus(lk).toString()
                         << " source:" << getSource()
                         << " db cloners remaining:" << _stats.databasesToClone
                         << " db cloners completed:" << _stats.databasesCloned;
}

void AllDatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("databasesToClone", static_cast<long long>(databasesToClone));
    builder->appendNumber("databasesCloned", static_cast<long long>(databasesCloned));
    for (auto&& db : databaseStats) {
        BSONObjBuilder dbBuilder(builder->subobjStart(
            DatabaseNameUtil::serialize(db.dbname, SerializationContext::stateDefault())));
        db.append(&dbBuilder);
        dbBuilder.doneFast();
    }
}

}  // namespace repl
}  // namespace mongo
