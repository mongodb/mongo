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

#include "mongo/s/config_upgrade.h"

#include <boost/function.hpp>

#include "mongo/base/init.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/distlock.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/mongo_version_range.h"
#include "mongo/s/type_config_version.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version.h"

namespace mongo {

    using mongoutils::str::stream;

    //
    // BEGIN CONFIG UPGRADE REGISTRATION
    //

    struct VersionRange {

        VersionRange(int _minCompatibleVersion, int _currentVersion) :
                minCompatibleVersion(_minCompatibleVersion), currentVersion(_currentVersion)
        {
        }

        bool operator==(const VersionRange& other) const {
            return (other.minCompatibleVersion == minCompatibleVersion)
                   && (other.currentVersion == currentVersion);
        }

        bool operator!=(const VersionRange& other) const {
            return !(*this == other);
        }

        int minCompatibleVersion;
        int currentVersion;
    };

    /**
     * Encapsulates the information needed to register a config upgrade.
     */
    struct Upgrade {

        typedef boost::function<bool(const ConnectionString&, const VersionType&, string*)> UpgradeCallback;

        Upgrade(int _fromVersion,
                const VersionRange& _toVersionRange,
                UpgradeCallback _upgradeCallback) :
                fromVersion(_fromVersion),
                toVersionRange(_toVersionRange),
                upgradeCallback(_upgradeCallback)
        {
        }

        // The config version we're upgrading from
        int fromVersion;
        // The config version we're upgrading to and the min compatible config version (min, to)
        VersionRange toVersionRange;
        // The upgrade callback which performs the actual upgrade
        UpgradeCallback upgradeCallback;
    };

    typedef map<int, Upgrade> ConfigUpgradeRegistry;
    void validateRegistry(const ConfigUpgradeRegistry& registry);

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

        // v0 to v5
        Upgrade v0ToV5(0, VersionRange(4, 5), doUpgradeV0ToV5);
        registry.insert(make_pair(v0ToV5.fromVersion, v0ToV5));

        // v4 to v5
        Upgrade v4ToV5(4, VersionRange(4, 5), doUpgradeV4ToV5);
        registry.insert(make_pair(v4ToV5.fromVersion, v4ToV5));

        validateRegistry(registry);

        return registry;
    }

    /**
     * Does a sanity-check validation of the registry ensuring three things:
     * 1. All upgrade paths lead to the same minCompatible/currentVersion
     * 2. Our constants match this final version pair
     * 3. There is a zero-version upgrade path
     */
    void validateRegistry(const ConfigUpgradeRegistry& registry) {

        VersionRange maxCompatibleConfigVersionRange(-1, -1);
        bool hasZeroVersionUpgrade = false;

        for (ConfigUpgradeRegistry::const_iterator it = registry.begin(); it != registry.end();
                ++it)
        {
            const Upgrade& upgrade = it->second;

            if (upgrade.fromVersion == 0) hasZeroVersionUpgrade = true;

            if (maxCompatibleConfigVersionRange.currentVersion
                < upgrade.toVersionRange.currentVersion)
            {
                maxCompatibleConfigVersionRange = upgrade.toVersionRange;
            }
            else if (maxCompatibleConfigVersionRange.currentVersion
                     == upgrade.toVersionRange.currentVersion)
            {
                // Make sure all max upgrade paths end up with same version and compatibility
                fassert(16621, maxCompatibleConfigVersionRange == upgrade.toVersionRange);
            }
        }

        // Make sure we have a zero-version upgrade
        fassert(16622, hasZeroVersionUpgrade);

        // Make sure our max registered range is the same as our constants
        fassert(16623,
                maxCompatibleConfigVersionRange
                == VersionRange(MIN_COMPATIBLE_CONFIG_VERSION, CURRENT_CONFIG_VERSION));
    }

    //
    // END CONFIG UPGRADE REGISTRATION
    //

    // Gets the config version information from the config server
    Status getConfigVersion(const ConnectionString& configLoc, VersionType* versionInfo) {
        try {
            versionInfo->clear();

            ScopedDbConnection conn(configLoc, 30);

            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query("config.version",
                                                                      BSONObj())));

            bool hasConfigData = conn->count(ShardType::ConfigNS)
                                 || conn->count(DatabaseType::ConfigNS)
                                 || conn->count(CollectionType::ConfigNS);

            if (!cursor->more()) {

                // Version is 1 if we have data, 0 if we're completely empty
                if (hasConfigData) {
                    versionInfo->setMinCompatibleVersion(UpgradeHistory_UnreportedVersion);
                    versionInfo->setCurrentVersion(UpgradeHistory_UnreportedVersion);
                }
                else {
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
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        return Status::OK();
    }

    // Checks version compatibility with our version
    VersionStatus isConfigVersionCompatible(const VersionType& versionInfo, string* whyNot) {

        string dummy;
        if (!whyNot) whyNot = &dummy;

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
            !MongoVersionRange::parseBSONArray(versionInfo.getExcludingMongoVersions(),
                                               &excludedRanges,
                                               whyNot))
        {

            *whyNot = stream() << "could not understand excluded version ranges"
                               << causedBy(whyNot);

            return VersionStatus_Incompatible;
        }

        // versionString is the global version of this process
        if (isInMongoVersionRanges(versionString, excludedRanges)) {

            // Cast needed here for MSVC compiler issue
            *whyNot = stream() << "not compatible with current config version, version "
                               << reinterpret_cast<const char*>(versionString)
                               << "has been excluded.";

            return VersionStatus_Incompatible;
        }

        // Check if we need to upgrade
        if (versionInfo.getCurrentVersion() >= CURRENT_CONFIG_VERSION) {
            return VersionStatus_Compatible;
        }

        return VersionStatus_NeedUpgrade;
    }

    // Returns true if we can confirm the balancer is stopped
    bool _isBalancerStopped(const ConnectionString& configLoc, string* errMsg) {
        
        // Get the balancer information
        BSONObj balancerDoc;
        try {
            ScopedDbConnection conn(configLoc, 30);
            balancerDoc = conn->findOne(SettingsType::ConfigNS,
                                        BSON(SettingsType::key("balancer")));
            conn.done();
        }
        catch (const DBException& e) {
            *errMsg = e.toString();
            return false;
        }
        
        return balancerDoc[SettingsType::balancerStopped()].trueValue();
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
            }
            else {
                resultOk = conn->runCommand("admin", BSON( "fsync" << 1 ), result); 
            }
            conn.done();
        }
        catch (const DBException& e) {
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
    bool _nextUpgrade(const ConnectionString& configLoc,
                      const ConfigUpgradeRegistry& registry,
                      const VersionType& lastVersionInfo,
                      VersionType* upgradedVersionInfo,
                      string* errMsg)
    {
        int fromVersion = lastVersionInfo.getCurrentVersion();

        ConfigUpgradeRegistry::const_iterator foundIt = registry.find(fromVersion);

        if (foundIt == registry.end()) {

            *errMsg = stream() << "newer version " << CURRENT_CONFIG_VERSION
                               << " of mongo config metadata is required, " << "current version is "
                               << fromVersion << ", "
                               << "don't know how to upgrade from this version";

            return false;
        }

        const Upgrade& upgrade = foundIt->second;
        int toVersion = upgrade.toVersionRange.currentVersion;

        log() << "starting next upgrade step from v" << fromVersion << " to v" << toVersion << endl;

        // Log begin to config.changelog
        Status logStatus = logConfigChange(configLoc,
                                           "",
                                           VersionType::ConfigNS,
                                           "starting upgrade of config database",
                                           BSON("from" << fromVersion << "to" << toVersion));

        if (!logStatus.isOK()) {

            *errMsg = stream() << "could not write initial changelog entry for upgrade"
                               << causedBy(logStatus);

            return false;
        }

        if (!upgrade.upgradeCallback(configLoc, lastVersionInfo, errMsg)) {

            *errMsg = stream() << "error upgrading config database from v" << fromVersion << " to v"
                               << toVersion << causedBy(errMsg);

            return false;
        }

        // Get the config version we've upgraded to and make sure it's sane
        Status verifyConfigStatus = getConfigVersion(configLoc, upgradedVersionInfo);

        if (!verifyConfigStatus.isOK()) {

            *errMsg = stream() << "failed to validate v" << fromVersion << " config version upgrade"
                               << causedBy(verifyConfigStatus);

            return false;
        }

        // Log end to config.changelog
        logStatus = logConfigChange(configLoc,
                                    "",
                                    VersionType::ConfigNS,
                                    "finished upgrade of config database",
                                    BSON("from" << fromVersion << "to" << toVersion));

        if (!logStatus.isOK()) {

            *errMsg = stream() << "could not write final changelog entry for upgrade"
                               << causedBy(logStatus);

            return false;
        }

        return true;
    }

    // Upgrades the config server
    bool checkAndUpgradeConfigVersion(const ConnectionString& configLoc,
                                      bool upgrade,
                                      VersionType* initialVersionInfo,
                                      VersionType* versionInfo,
                                      string* errMsg)
    {
        string dummy;
        if (!errMsg) errMsg = &dummy;

        //
        // Check compatibility of config version
        //

        Status getConfigStatus = getConfigVersion(configLoc, versionInfo);

        if (!getConfigStatus.isOK()) {

            *errMsg = stream() << "could not load config version for upgrade"
                               << causedBy(getConfigStatus);

            return false;
        }

        versionInfo->cloneTo(initialVersionInfo);

        VersionStatus comp = isConfigVersionCompatible(*versionInfo, errMsg);

        if (comp == VersionStatus_Incompatible) return false;
        if (comp == VersionStatus_Compatible) return true;
        verify(comp == VersionStatus_NeedUpgrade);

        //
        // Our current config version is now greater than the current version, so we should upgrade
        // if possible.
        //

        // The first empty version is technically an upgrade, but has special semantics
        bool isEmptyVersion = versionInfo->getCurrentVersion() == UpgradeHistory_EmptyVersion;

        // First check for the upgrade flag (but no flag is needed if we're upgrading from empty)
        if (!isEmptyVersion && !upgrade) {

            *errMsg = stream() << "newer version " << CURRENT_CONFIG_VERSION
                               << " of mongo config metadata is required, " << "current version is "
                               << versionInfo->getCurrentVersion() << ", "
                               << "need to run mongos with --upgrade";

            return false;
        }

        // Contact the config servers to make sure all are online - otherwise we wait a long time
        // for locks.
        if (!_checkConfigServersAlive(configLoc, errMsg)) {

            if (isEmptyVersion) {
                *errMsg = stream() << "all config servers must be reachable for initial"
                                   << " config database creation" << causedBy(errMsg);
            }
            else {
                *errMsg = stream() << "all config servers must be reachable for config upgrade"
                                   << causedBy(errMsg);
            }
            
            return false;
        }

        // Check whether or not the balancer is online, if it is online we will not upgrade
        // (but we will initialize the config server)
        if (!isEmptyVersion && !_isBalancerStopped(configLoc, errMsg)) {
            
            *errMsg = stream() << "balancer must be stopped for config upgrade"
                               << causedBy(errMsg);
            
            return false;
        }

        //
        // Acquire a lock for the upgrade process.
        //
        // We want to ensure that only a single mongo process is upgrading the config server at a
        // time.
        //

        ScopedDistributedLock upgradeLock(configLoc, "configUpgrade");
        upgradeLock.setLockMessage(stream() << "upgrading config database to new format v"
                                            << CURRENT_CONFIG_VERSION);

        if (!upgradeLock.acquire(20 * 60 * 1000, errMsg)) {

            *errMsg = stream() << "could not acquire upgrade lock for config upgrade to v"
                               << CURRENT_CONFIG_VERSION << causedBy(errMsg);

            return false;
        }

        //
        // Double-check compatibility inside the upgrade lock
        // Another process may have won the lock earlier and done the upgrade for us, check
        // if this is the case.
        //

        getConfigStatus = getConfigVersion(configLoc, versionInfo);

        if (!getConfigStatus.isOK()) {

            *errMsg = stream() << "could not reload config version for upgrade"
                               << causedBy(getConfigStatus);

            return false;
        }

        versionInfo->cloneTo(initialVersionInfo);

        comp = isConfigVersionCompatible(*versionInfo, errMsg);

        if (comp == VersionStatus_Incompatible) return false;
        if (comp == VersionStatus_Compatible) return true;
        verify(comp == VersionStatus_NeedUpgrade);

        //
        // Run through the upgrade steps necessary to bring our config version to the current
        // version
        //

        log() << "starting upgrade of config server from v" << versionInfo->getCurrentVersion()
              << " to v" << CURRENT_CONFIG_VERSION << endl;

        ConfigUpgradeRegistry registry(createRegistry());

        while (versionInfo->getCurrentVersion() < CURRENT_CONFIG_VERSION) {

            int fromVersion = versionInfo->getCurrentVersion();

            //
            // Run the next upgrade process and replace versionInfo with the result of the
            // upgrade.
            //

            if (!_nextUpgrade(configLoc, registry, *versionInfo, versionInfo, errMsg)) {
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

        verify(versionInfo->getCurrentVersion() == CURRENT_CONFIG_VERSION);

        log() << "upgrade of config server to v" << versionInfo->getCurrentVersion()
              << " successful" << endl;

        return true;
    }

}
