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

#pragma once

#include "mongo/client/dbclientinterface.h"
#include "mongo/s/type_config_version.h"

namespace mongo {

    /**
     *
     * UPGRADE HISTORY
     *
     * The enum below documents the version changes to *both* the config server data layout
     * and the versioning protocol between clients (i.e. the set of calls between mongos and
     * mongod).
     *
     * Friendly notice:
     *
     * EVERY CHANGE EITHER IN CONFIG LAYOUT AND IN S/D PROTOCOL MUST BE RECORDED HERE BY AN INCREASE
     * IN THE VERSION AND BY TAKING THE FOLLOWING STEPS. (IF YOU DON'T UNDERSTAND THESE STEPS, YOU
     * SHOULD PROBABLY NOT BE UPGRADING THE VERSIONS BY YOURSELF.)
     *
     * + A new entry in the UpgradeHistory enum is created
     * + The CURRENT_CONFIG_VERSION below is incremented to that version
     * + There should be a determination if the MIN_COMPATIBLE_CONFIG_VERSION should be increased or
     *   not. This means determining if, by introducing the changes to layout and/or protocol, the
     *   new mongos/d can co-exist in a cluster with the old ones.
     * + If layout changes are involved, there should be a corresponding layout upgrade routine. See
     *   for instance config_upgrade_vX_to_vY.cpp.
     * + Again, if a layout change occurs, the base upgrade method, config_upgrade_v0_to_vX.cpp must
     *   be upgraded. This means that all new clusters will start at the newest versions.
     *
     */
    enum UpgradeHistory {

        /**
         * The empty version, reported when there is no config server data
         */
        UpgradeHistory_EmptyVersion = 0,

        /**
         * The unreported version older mongoses used before config.version collection existed
         *
         * If there is a config.shards/databases/collections collection but no config.version
         * collection, version 1 is assumed
         */
        UpgradeHistory_UnreportedVersion = 1,

        /**
         * NOTE: We skip version 2 here since it is very old and we shouldn't see it in the wild.
         *
         * Do not skip upgrade versions in the future.
         */

        /**
         * Base version used by pre-2.4 mongoses with no collection epochs.
         */
        UpgradeHistory_NoEpochVersion = 3,

        /**
         * Version upgrade which added collection epochs to all sharded collections and
         * chunks.
         *
         * Also:
         * + Version document in config.version now of the form:
         *   { minVersion : X, currentVersion : Y, clusterId : OID(...) }
         * + Mongos pings include a "mongoVersion" field indicating the mongos version
         * + Mongos pings include a "configVersion" field indicating the current config version
         * + Mongos explicitly ignores any collection with a "primary" field
         */
        UpgradeHistory_MandatoryEpochVersion = 4,

        /**
         * Version upgrade with the following changes:
         *
         * Semantic changes:
         * + Dropping a collection from mongos now waits for the chunks to be removed from the
         *   config server before contacting each shard.
         * + Version epoch comparison is now strict. OID(000...) is no longer a "wildcard" that
         *   matches everything. (TODO)
         * + config.collections lastmod now displays the timestamp correctly.
         * + initShardVersion can now return isMaster output, which contains remote version
         *   information. (TODO)
         *
         * Syntax:
         * + Replaced lastModEpoch with epoch in config.collections. (TODO)
         * + Deprecated version field in config.versions. (TODO)
         * + Deprecated "name" and "sharded" fields in config.databases. (TODO)
         * + Added additional info to changelog entries for migration.
         * + Added an optional "draining" field to config.databases and config.collections. (TODO)
         * + Added "host" field to config.locks.
         * + Added an optional "pending" to config.chunks.
         */
        UpgradeHistory_StrictEpochVersion = 5
    };

    //
    // CURRENT VERSION CONSTANTS
    // Note: We must modify these constants we add new upgrades, otherwise we will fail on startup
    //

    // Earliest version we're compatible with
    const int MIN_COMPATIBLE_CONFIG_VERSION = UpgradeHistory_MandatoryEpochVersion;

    // Latest version we know how to communicate with
    const int CURRENT_CONFIG_VERSION = UpgradeHistory_StrictEpochVersion;

    //
    // DECLARATION OF UPGRADE FUNCTIONALITY
    // These functions must also be wired explicitly to the upgrade path in
    // config_upgrade.cpp::createRegistry()
    //

    bool doUpgradeV0ToV5(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg);

    bool doUpgradeV4ToV5(const ConnectionString& configLoc,
                         const VersionType& lastVersionInfo,
                         string* errMsg);

    //
    // Utilities for upgrading a config database to a new config version and checking the status of
    // the config version.
    //

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
    VersionStatus isConfigVersionCompatible(const VersionType& versionInfo, string* whyNot);

    /**
     * Returns the config version of the cluster pointed at by the connection string.
     *
     * @return OK if version found successfully, error status if something bad happened.
     */
    Status getConfigVersion(const ConnectionString& configLoc, VersionType* versionInfo);

    /**
     * Checks the config version and ensures it's the latest version, otherwise tries to update.
     *
     * @return true if the config version is now compatible.
     * @return initial and finalVersionInfo indicating the start and end versions of the upgrade.
     *         These are the same if no upgrade occurred.
     */
    bool checkAndUpgradeConfigVersion(const ConnectionString& configLoc,
                                      bool upgrade,
                                      VersionType* initialVersionInfo,
                                      VersionType* finalVersionInfo,
                                      string* errMsg);

} // end namespace
