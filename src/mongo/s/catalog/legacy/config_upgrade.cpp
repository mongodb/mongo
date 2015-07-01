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
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/catalog/legacy/mongo_version_range.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
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

// Implemented in the respective steps' .cpp file
bool doUpgradeV0ToV7(CatalogManager* catalogManager,
                     const VersionType& lastVersionInfo,
                     std::string* errMsg);

bool doUpgradeV6ToV7(CatalogManager* catalogManager,
                     const VersionType& lastVersionInfo,
                     std::string* errMsg);

namespace {

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
 * Encapsulates the information needed to register a config upgrade.
 */
struct UpgradeStep {
    typedef stdx::function<bool(CatalogManager*, const VersionType&, string*)> UpgradeCallback;

    UpgradeStep(int _fromVersion,
                const VersionRange& _toVersionRange,
                UpgradeCallback _upgradeCallback)
        : fromVersion(_fromVersion),
          toVersionRange(_toVersionRange),
          upgradeCallback(_upgradeCallback) {}

    // The config version we're upgrading from
    int fromVersion;

    // The config version we're upgrading to and the min compatible config version (min, to)
    VersionRange toVersionRange;

    // The upgrade callback which performs the actual upgrade
    UpgradeCallback upgradeCallback;
};

typedef map<int, UpgradeStep> ConfigUpgradeRegistry;

/**
 * Does a sanity-check validation of the registry ensuring three things:
 * 1. All upgrade paths lead to the same minCompatible/currentVersion
 * 2. Our constants match this final version pair
 * 3. There is a zero-version upgrade path
 */
void validateRegistry(const ConfigUpgradeRegistry& registry) {
    VersionRange maxCompatibleConfigVersionRange(-1, -1);
    bool hasZeroVersionUpgrade = false;

    for (const auto& upgradeStep : registry) {
        const UpgradeStep& upgrade = upgradeStep.second;

        if (upgrade.fromVersion == 0) {
            hasZeroVersionUpgrade = true;
        }

        if (maxCompatibleConfigVersionRange.currentVersion <
            upgrade.toVersionRange.currentVersion) {
            maxCompatibleConfigVersionRange = upgrade.toVersionRange;
        } else if (maxCompatibleConfigVersionRange.currentVersion ==
                   upgrade.toVersionRange.currentVersion) {
            // Make sure all max upgrade paths end up with same version and compatibility
            fassert(16621, maxCompatibleConfigVersionRange == upgrade.toVersionRange);
        }
    }

    // Make sure we have a zero-version upgrade
    fassert(16622, hasZeroVersionUpgrade);

    // Make sure our max registered range is the same as our constants
    fassert(16623,
            maxCompatibleConfigVersionRange ==
                VersionRange(MIN_COMPATIBLE_CONFIG_VERSION, CURRENT_CONFIG_VERSION));
}

/**
 * Creates a registry of config upgrades used by the code below.
 *
 * MODIFY THIS CODE HERE TO CREATE A NEW UPGRADE PATH FROM X to Y
 * YOU MUST ALSO MODIFY THE VERSION DECLARATIONS IN config_upgrade.h
 *
 * Caveats:
 * - All upgrade paths must eventually lead to the exact same version range of
 * min and max compatible versions.
 * - This resulting version range must be equal to:
 * make_pair(MIN_COMPATIBLE_CONFIG_VERSION, CURRENT_CONFIG_VERSION)
 * - There must always be an upgrade path from the empty version (0) to the latest
 * config version.
 *
 * If any of the above is false, we fassert and fail to start.
 */
ConfigUpgradeRegistry createRegistry() {
    ConfigUpgradeRegistry registry;

    // v0 to v7
    UpgradeStep v0ToV7(0, VersionRange(6, 7), doUpgradeV0ToV7);
    registry.insert(make_pair(v0ToV7.fromVersion, v0ToV7));

    // v6 to v7
    UpgradeStep v6ToV7(6, VersionRange(6, 7), doUpgradeV6ToV7);
    registry.insert(make_pair(v6ToV7.fromVersion, v6ToV7));

    validateRegistry(registry);

    return registry;
}

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

    // Check that the mongo version of this process hasn't been excluded from the cluster
    vector<MongoVersionRange> excludedRanges;
    if (versionInfo.isExcludingMongoVersionsSet() &&
        !MongoVersionRange::parseBSONArray(
            versionInfo.getExcludingMongoVersions(), &excludedRanges, whyNot)) {
        *whyNot = stream() << "could not understand excluded version ranges" << causedBy(whyNot);

        return VersionStatus_Incompatible;
    }

    // versionString is the global version of this process
    if (isInMongoVersionRanges(versionString, excludedRanges)) {
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
bool _checkConfigServersAlive(const ConnectionString& configLoc, string* errMsg) {
    bool resultOk;
    BSONObj result;
    try {
        ScopedDbConnection conn(configLoc, 30);
        if (conn->type() == ConnectionString::SYNC) {
            // TODO: Dynamic cast is bad, we need a better way of managing this op
            // via the heirarchy (or not)
            SyncClusterConnection* scc = dynamic_cast<SyncClusterConnection*>(conn.get());
            fassert(16729, scc != NULL);
            return scc->prepare(*errMsg);
        } else {
            resultOk = conn->runCommand("admin", BSON("fsync" << 1), result);
        }
        conn.done();
    } catch (const DBException& e) {
        *errMsg = e.toString();
        return false;
    }

    if (!resultOk) {
        *errMsg = DBClientWithCommands::getLastErrorString(result);
        return false;
    }

    return true;
}

// Dispatches upgrades based on version to the upgrades registered in the upgrade registry
bool _nextUpgrade(CatalogManager* catalogManager,
                  const ConfigUpgradeRegistry& registry,
                  const VersionType& lastVersionInfo,
                  VersionType* upgradedVersionInfo,
                  string* errMsg) {
    int fromVersion = lastVersionInfo.getCurrentVersion();

    ConfigUpgradeRegistry::const_iterator foundIt = registry.find(fromVersion);

    if (foundIt == registry.end()) {
        *errMsg = stream() << "newer version " << CURRENT_CONFIG_VERSION
                           << " of mongo config metadata is required, "
                           << "current version is " << fromVersion << ", "
                           << "don't know how to upgrade from this version";

        return false;
    }

    const UpgradeStep& upgrade = foundIt->second;
    int toVersion = upgrade.toVersionRange.currentVersion;

    log() << "starting next upgrade step from v" << fromVersion << " to v" << toVersion;

    // Log begin to config.changelog
    catalogManager->logChange("<upgrade>",
                              "starting upgrade of config database",
                              VersionType::ConfigNS,
                              BSON("from" << fromVersion << "to" << toVersion));

    if (!upgrade.upgradeCallback(catalogManager, lastVersionInfo, errMsg)) {
        *errMsg = stream() << "error upgrading config database from v" << fromVersion << " to v"
                           << toVersion << causedBy(errMsg);
        return false;
    }

    // Get the config version we've upgraded to and make sure it's sane
    Status verifyConfigStatus = getConfigVersion(catalogManager, upgradedVersionInfo);

    if (!verifyConfigStatus.isOK()) {
        *errMsg = stream() << "failed to validate v" << fromVersion << " config version upgrade"
                           << causedBy(verifyConfigStatus);

        return false;
    }

    catalogManager->logChange("<upgrade>",
                              "finished upgrade of config database",
                              VersionType::ConfigNS,
                              BSON("from" << fromVersion << "to" << toVersion));
    return true;
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

        ScopedDbConnection conn(catalogManager->connectionString(), 30);

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
        string errMsg;

        if (!versionInfo->parseBSON(versionDoc, &errMsg) || !versionInfo->isValid(&errMsg)) {
            conn.done();

            return Status(ErrorCodes::UnsupportedFormat,
                          stream() << "invalid config version document " << versionDoc
                                   << causedBy(errMsg));
        }

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

bool checkAndUpgradeConfigVersion(CatalogManager* catalogManager,
                                  bool upgrade,
                                  VersionType* initialVersionInfo,
                                  VersionType* versionInfo,
                                  string* errMsg) {
    string dummy;
    if (!errMsg) {
        errMsg = &dummy;
    }

    Status getConfigStatus = getConfigVersion(catalogManager, versionInfo);
    if (!getConfigStatus.isOK()) {
        *errMsg = stream() << "could not load config version for upgrade"
                           << causedBy(getConfigStatus);
        return false;
    }

    versionInfo->cloneTo(initialVersionInfo);

    VersionStatus comp = isConfigVersionCompatible(*versionInfo, errMsg);

    if (comp == VersionStatus_Incompatible)
        return false;
    if (comp == VersionStatus_Compatible)
        return true;

    invariant(comp == VersionStatus_NeedUpgrade);

    //
    // Our current config version is now greater than the current version, so we should upgrade
    // if possible.
    //

    // The first empty version is technically an upgrade, but has special semantics
    bool isEmptyVersion = versionInfo->getCurrentVersion() == UpgradeHistory_EmptyVersion;

    // First check for the upgrade flag (but no flag is needed if we're upgrading from empty)
    if (!isEmptyVersion && !upgrade) {
        *errMsg = stream() << "newer version " << CURRENT_CONFIG_VERSION
                           << " of mongo config metadata is required, "
                           << "current version is " << versionInfo->getCurrentVersion() << ", "
                           << "need to run mongos with --upgrade";

        return false;
    }

    // Contact the config servers to make sure all are online - otherwise we wait a long time
    // for locks.
    if (!_checkConfigServersAlive(catalogManager->connectionString(), errMsg)) {
        if (isEmptyVersion) {
            *errMsg = stream() << "all config servers must be reachable for initial"
                               << " config database creation" << causedBy(errMsg);
        } else {
            *errMsg = stream() << "all config servers must be reachable for config upgrade"
                               << causedBy(errMsg);
        }

        return false;
    }

    // Check whether or not the balancer is online, if it is online we will not upgrade
    // (but we will initialize the config server)
    if (!isEmptyVersion) {
        auto balSettingsResult = catalogManager->getGlobalSettings(SettingsType::BalancerDocKey);
        if (balSettingsResult.isOK()) {
            SettingsType balSettings = balSettingsResult.getValue();
            if (!balSettings.getBalancerStopped()) {
                *errMsg = stream() << "balancer must be stopped for config upgrade"
                                   << causedBy(errMsg);
            }
        }
    }

    //
    // Acquire a lock for the upgrade process.
    //
    // We want to ensure that only a single mongo process is upgrading the config server at a
    // time.
    //

    string whyMessage(stream() << "upgrading config database to new format v"
                               << CURRENT_CONFIG_VERSION);
    auto lockTimeout = stdx::chrono::milliseconds(20 * 60 * 1000);
    auto scopedDistLock =
        catalogManager->getDistLockManager()->lock("configUpgrade", whyMessage, lockTimeout);
    if (!scopedDistLock.isOK()) {
        *errMsg = scopedDistLock.getStatus().toString();
        return false;
    }

    //
    // Double-check compatibility inside the upgrade lock
    // Another process may have won the lock earlier and done the upgrade for us, check
    // if this is the case.
    //

    getConfigStatus = getConfigVersion(catalogManager, versionInfo);
    if (!getConfigStatus.isOK()) {
        *errMsg = stream() << "could not reload config version for upgrade"
                           << causedBy(getConfigStatus);
        return false;
    }

    versionInfo->cloneTo(initialVersionInfo);

    comp = isConfigVersionCompatible(*versionInfo, errMsg);

    if (comp == VersionStatus_Incompatible)
        return false;
    if (comp == VersionStatus_Compatible)
        return true;

    invariant(comp == VersionStatus_NeedUpgrade);

    //
    // Run through the upgrade steps necessary to bring our config version to the current
    // version
    //

    log() << "starting upgrade of config server from v" << versionInfo->getCurrentVersion()
          << " to v" << CURRENT_CONFIG_VERSION;

    ConfigUpgradeRegistry registry(createRegistry());

    while (versionInfo->getCurrentVersion() < CURRENT_CONFIG_VERSION) {
        int fromVersion = versionInfo->getCurrentVersion();

        //
        // Run the next upgrade process and replace versionInfo with the result of the
        // upgrade.
        //

        if (!_nextUpgrade(catalogManager, registry, *versionInfo, versionInfo, errMsg)) {
            return false;
        }

        // Ensure we're making progress here
        if (versionInfo->getCurrentVersion() <= fromVersion) {
            *errMsg = stream() << "bad v" << fromVersion << " config version upgrade, "
                               << "version did not increment and is now "
                               << versionInfo->getCurrentVersion();

            return false;
        }
    }

    invariant(versionInfo->getCurrentVersion() == CURRENT_CONFIG_VERSION);

    log() << "upgrade of config server to v" << versionInfo->getCurrentVersion() << " successful";

    return true;
}

}  // namespace mongo
