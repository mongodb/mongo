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

#pragma once

namespace mongo {

/**
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
     * + Dropping a collection from mongos now waits for the chunks to be removed from the
     *   config server before contacting each shard. Because of this, mongos should be
     *   upgraded first before mongod or never drop collections during upgrade.
     */
    UpgradeHistory_DummyBumpPre2_6 = 5,

    /**
     * Version upgrade with the following changes:
     *
     * + "_secondaryThrottle" field for config.settings now accepts write concern
     *   specifications.
     * + config.locks { ts: 1 } index is no longer unique.
     */
    UpgradeHistory_DummyBumpPre2_8 = 6,  // Note: 2.8 is also known as 3.0.
};

// Earliest version we're compatible with
const int MIN_COMPATIBLE_CONFIG_VERSION = UpgradeHistory_DummyBumpPre2_6;

// Latest version we know how to communicate with
const int CURRENT_CONFIG_VERSION = UpgradeHistory_DummyBumpPre2_8;

}  // namespace mongo
