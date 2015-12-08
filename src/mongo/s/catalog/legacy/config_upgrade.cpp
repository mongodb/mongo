/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/config_upgrade.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/syncclusterconnection.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/catalog/mongo_version_range.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/version.h"

namespace mongo {

using std::unique_ptr;
using std::make_pair;
using std::map;
using std::string;
using std::vector;
using str::stream;

namespace {

Status makeConfigVersionDocument(OperationContext* txn, CatalogManager* catalogManager) {
    //
    // Even though the initial config write is a single-document update, that single document
    // is on multiple config servers and requests can interleave.  The upgrade lock prevents
    // this.
    //

    log() << "writing initial config version at v" << CURRENT_CONFIG_VERSION;

    OID newClusterId = OID::gen();

    VersionType versionInfo;

    // Upgrade to new version
    versionInfo.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
    versionInfo.setCurrentVersion(CURRENT_CONFIG_VERSION);
    versionInfo.setClusterId(newClusterId);

    invariantOK(versionInfo.validate());

    // If the cluster has not previously been initialized, we need to set the version before
    // using so subsequent mongoses use the config data the same way.  This requires all three
    // config servers online initially.
    auto status = catalogManager->updateConfigDocument(
        txn, VersionType::ConfigNS, BSON("_id" << 1), versionInfo.toBSON(), true);
    return status.getStatus();
}

struct VersionRange {
    VersionRange(int _minCompatibleVersion, int _currentVersion)
        : minCompatibleVersion(_minCompatibleVersion), currentVersion(_currentVersion) {}

    bool operator==(const VersionRange& other) const {
        return (other.minCompatibleVersion == minCompatibleVersion) &&
            (other.currentVersion == currentVersion);
    }

    bool operator!=(const VersionRange& other) const {
        return !(*this == other);
    }

    int minCompatibleVersion;
    int currentVersion;
};

enum VersionStatus {
    // No way to upgrade the test version to be compatible with current version
    VersionStatus_Incompatible,

    // Current version is compatible with test version
    VersionStatus_Compatible,

    // Test version must be upgraded to be compatible with current version
    VersionStatus_NeedUpgrade
};

/**
 * Checks whether or not a particular cluster version is compatible with our current
 * version and mongodb version.  The version is compatible if it falls between the
 * MIN_COMPATIBLE_CONFIG_VERSION and CURRENT_CONFIG_VERSION and is not explicitly excluded.
 *
 * @return a VersionStatus enum indicating compatibility
 */
VersionStatus isConfigVersionCompatible(const VersionType& versionInfo, string* whyNot) {
    string dummy;
    if (!whyNot) {
        whyNot = &dummy;
    }

    // Check if we're empty
    if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
        return VersionStatus_NeedUpgrade;
    }

    // Check that we aren't too old
    if (CURRENT_CONFIG_VERSION < versionInfo.getMinCompatibleVersion()) {
        *whyNot = stream() << "the config version " << CURRENT_CONFIG_VERSION
                           << " of our process is too old "
                           << "for the detected config version "
                           << versionInfo.getMinCompatibleVersion();

        return VersionStatus_Incompatible;
    }

    // versionString is the global version of this process
    if (isInMongoVersionRanges(versionString, versionInfo.getExcludingMongoVersions())) {
        // Cast needed here for MSVC compiler issue
        *whyNot = stream() << "not compatible with current config version, version "
                           << reinterpret_cast<const char*>(versionString) << "has been excluded.";

        return VersionStatus_Incompatible;
    }

    // Check if we need to upgrade
    if (versionInfo.getCurrentVersion() >= CURRENT_CONFIG_VERSION) {
        return VersionStatus_Compatible;
    }

    return VersionStatus_NeedUpgrade;
}

// Checks that all config servers are online
Status _checkConfigServersAlive(const ConnectionString& configLoc) {
    BSONObj result;
    try {
        if (configLoc.type() == ConnectionString::SYNC) {
            ScopedDbConnection conn(configLoc, 30);
            // TODO: Dynamic cast is bad, we need a better way of managing this op
            // via the heirarchy (or not)
            SyncClusterConnection* scc = dynamic_cast<SyncClusterConnection*>(conn.get());
            fassert(16729, scc != NULL);
            std::string errMsg;
            if (!scc->prepare(errMsg)) {
                return {ErrorCodes::HostUnreachable, errMsg};
            }
            conn.done();
            return Status::OK();
        } else {
            ScopedDbConnection conn(configLoc, 30);
            conn->runCommand("admin", BSON("fsync" << 1), result);
            conn.done();
            return getStatusFromCommandResult(result);
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

}  // namespace


/**
 * Returns the config version of the cluster pointed at by the connection string.
 *
 * @return OK if version found successfully, error status if something bad happened.
 */
Status getConfigVersion(CatalogManager* catalogManager, VersionType* versionInfo) {
    try {
        versionInfo->clear();

        ScopedDbConnection conn(grid.shardRegistry()->getConfigServerConnectionString(), 30);

        unique_ptr<DBClientCursor> cursor(_safeCursor(conn->query("config.version", BSONObj())));

        bool hasConfigData = conn->count(ShardType::ConfigNS) ||
            conn->count(DatabaseType::ConfigNS) || conn->count(CollectionType::ConfigNS);

        if (!cursor->more()) {
            // Version is 1 if we have data, 0 if we're completely empty
            if (hasConfigData) {
                versionInfo->setMinCompatibleVersion(UpgradeHistory_UnreportedVersion);
                versionInfo->setCurrentVersion(UpgradeHistory_UnreportedVersion);
            } else {
                versionInfo->setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
                versionInfo->setCurrentVersion(UpgradeHistory_EmptyVersion);
            }

            conn.done();
            return Status::OK();
        }

        BSONObj versionDoc = cursor->next();
        auto versionInfoResult = VersionType::fromBSON(versionDoc);
        if (!versionInfoResult.isOK()) {
            conn.done();

            return Status(ErrorCodes::UnsupportedFormat,
                          stream() << "invalid config version document " << versionDoc
                                   << versionInfoResult.getStatus().toString());
        }
        *versionInfo = versionInfoResult.getValue();

        if (cursor->more()) {
            conn.done();

            return Status(ErrorCodes::RemoteValidationError,
                          stream() << "should only have 1 document "
                                   << "in config.version collection");
        }
        conn.done();
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

Status checkAndInitConfigVersion(OperationContext* txn,
                                 CatalogManager* catalogManager,
                                 DistLockManager* distLockManager) {
    VersionType versionInfo;
    Status status = getConfigVersion(catalogManager, &versionInfo);
    if (!status.isOK()) {
        return status;
    }

    string errMsg;
    VersionStatus comp = isConfigVersionCompatible(versionInfo, &errMsg);

    if (comp == VersionStatus_Incompatible)
        return {ErrorCodes::IncompatibleShardingMetadata, errMsg};
    if (comp == VersionStatus_Compatible)
        return Status::OK();

    invariant(comp == VersionStatus_NeedUpgrade);

    if (versionInfo.getCurrentVersion() != UpgradeHistory_EmptyVersion) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                stream() << "newer version " << CURRENT_CONFIG_VERSION
                         << " of mongo config metadata is required, "
                         << "current version is " << versionInfo.getCurrentVersion()};
    }

    // Contact the config servers to make sure all are online - otherwise we wait a long time
    // for locks.
    status = _checkConfigServersAlive(grid.shardRegistry()->getConfigServerConnectionString());
    if (!status.isOK()) {
        return status;
    }

    //
    // Acquire a lock for the upgrade process.
    //
    // We want to ensure that only a single mongo process is upgrading the config server at a
    // time.
    //

    string whyMessage(stream() << "initializing config database to new format v"
                               << CURRENT_CONFIG_VERSION);
    auto lockTimeout = stdx::chrono::minutes(20);
    auto scopedDistLock = distLockManager->lock(txn, "configUpgrade", whyMessage, lockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    //
    // Double-check compatibility inside the upgrade lock
    // Another process may have won the lock earlier and done the upgrade for us, check
    // if this is the case.
    //

    status = getConfigVersion(catalogManager, &versionInfo);
    if (!status.isOK()) {
        return status;
    }

    comp = isConfigVersionCompatible(versionInfo, &errMsg);

    if (comp == VersionStatus_Incompatible) {
        return {ErrorCodes::IncompatibleShardingMetadata, errMsg};
    }
    if (comp == VersionStatus_Compatible)
        return Status::OK();

    invariant(comp == VersionStatus_NeedUpgrade);

    //
    // Run through the upgrade steps necessary to bring our config version to the current
    // version
    //

    log() << "initializing config server version to " << CURRENT_CONFIG_VERSION;

    status = makeConfigVersionDocument(txn, catalogManager);
    if (!status.isOK())
        return status;

    log() << "initialization of config server to v" << CURRENT_CONFIG_VERSION << " successful";

    return Status::OK();
}

}  // namespace mongo
