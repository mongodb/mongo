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

#include "mongo/dbtests/config_server_fixture.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/catalog/legacy/config_upgrade.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version.h"

namespace mongo {

using std::string;

namespace {

/**
 * Specialization of the config server fixture with helpers for the tests below.
 */
class ConfigUpgradeFixture : public ConfigServerFixture {
public:
    void stopBalancer() {
        // Note: The balancer key is needed in the update portion, for some reason related to
        // DBDirectClient
        DBDirectClient client(&_txn);
        client.update(SettingsType::ConfigNS,
                      BSON(SettingsType::key(SettingsType::BalancerDocKey)),
                      BSON(SettingsType::key(SettingsType::BalancerDocKey)
                           << SettingsType::balancerStopped(true)),
                      true,
                      false);
    }

    /**
     * Stores a legacy { version : X } config server entry
     */
    void storeLegacyConfigVersion(int version) {
        if (version == 0)
            return;

        DBDirectClient client(&_txn);

        if (version == 1) {
            ShardType shard;
            shard.setName("test");
            shard.setHost("$dummy:10000");
            client.insert(ShardType::ConfigNS, shard.toBSON());
            return;
        }

        client.insert(VersionType::ConfigNS, BSON("_id" << 1 << "version" << version));
    }

    VersionType loadLegacyConfigVersion() {
        DBDirectClient client(&_txn);
        return unittest::assertGet(
            VersionType::fromBSON(client.findOne(VersionType::ConfigNS, BSONObj())));
    }

    /**
     * Stores a newer { version, minVersion, currentVersion, clusterId } config server entry
     */
    void storeConfigVersion(const VersionType& versionInfo) {
        DBDirectClient client(&_txn);
        client.insert(VersionType::ConfigNS, versionInfo.toBSON());
    }

    /**
     * Stores a newer { version, minVersion, currentVersion, clusterId } config server entry.
     *
     * @return clusterId
     */
    OID storeConfigVersion(int configVersion) {
        if (configVersion < CURRENT_CONFIG_VERSION) {
            storeLegacyConfigVersion(configVersion);
            return OID();
        }

        VersionType version;
        version.setMinCompatibleVersion(configVersion);
        version.setCurrentVersion(configVersion);

        OID clusterId = OID::gen();

        version.setClusterId(clusterId);

        storeConfigVersion(version);
        return clusterId;
    }

    /**
     * Stores sample shard and ping information at the current version.
     */
    void storeShardsAndPings(int numShards, int numPings) {
        DBDirectClient client(&_txn);

        for (int i = 0; i < numShards; i++) {
            ShardType shard;
            shard.setName(OID::gen().toString());
            shard.setHost((string)(str::stream() << "$dummyShard:" << (i + 1) << "0000"));

            client.insert(ShardType::ConfigNS, shard.toBSON());
        }

        time_t started = time(0);
        for (int i = 0; i < numPings; i++) {
            MongosType ping;
            ping.setName((string)(str::stream() << "$dummyMongos:" << (i + 1) << "0000"));
            ping.setPing(jsTime());
            ping.setUptime(time(0) - started);
            ping.setWaiting(false);
            ping.setMongoVersion(versionString);
            ping.setConfigVersion(CURRENT_CONFIG_VERSION);

            if (i % 2 == 0) {
                ping.setPing(ping.getPing() - Minutes(10));
            }

            client.insert(MongosType::ConfigNS, ping.toBSON());
        }
    }
};

//
// Tests for upgrading the config server between versions.
//
// In general these tests do pretty minimal validation of the config server data itself, but
// do ensure that the upgrade mechanism is working correctly w.r.t the config.version
// collection.
//

// Rename the fixture so that our tests have a useful name in the executable
typedef ConfigUpgradeFixture ConfigUpgradeTests;

TEST_F(ConfigUpgradeTests, EmptyVersion) {
    //
    // Tests detection of empty config version
    //

    // Zero version (no version doc)
    VersionType oldVersion;
    Status status = getConfigVersion(grid.catalogManager(&_txn), &oldVersion);
    ASSERT(status.isOK());

    ASSERT_EQUALS(oldVersion.getMinCompatibleVersion(), 0);
    ASSERT_EQUALS(oldVersion.getCurrentVersion(), 0);
}

TEST_F(ConfigUpgradeTests, ClusterIDVersion) {
    //
    // Tests detection of newer config versions
    //

    VersionType newVersion;
    newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
    newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);
    storeConfigVersion(newVersion);

    newVersion.clear();

    // Current Version w/o clusterId (invalid!)
    Status status = getConfigVersion(grid.catalogManager(&_txn), &newVersion);
    ASSERT(!status.isOK());

    newVersion.clear();

    OID clusterId = OID::gen();
    newVersion.setClusterId(clusterId);
    newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
    newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

    clearVersion();
    storeConfigVersion(newVersion);

    newVersion.clear();

    // Current version w/ clusterId (valid!)
    status = getConfigVersion(grid.catalogManager(&_txn), &newVersion);
    ASSERT(status.isOK());

    ASSERT_EQUALS(newVersion.getMinCompatibleVersion(), MIN_COMPATIBLE_CONFIG_VERSION);
    ASSERT_EQUALS(newVersion.getCurrentVersion(), CURRENT_CONFIG_VERSION);
    ASSERT_EQUALS(newVersion.getClusterId(), clusterId);
}

TEST_F(ConfigUpgradeTests, InitialUpgrade) {
    //
    // Tests initializing the config server to the initial version
    //

    string errMsg;
    ASSERT_OK(grid.catalogManager(&_txn)->initConfigVersion());

    VersionType version;
    ASSERT_OK(getConfigVersion(grid.catalogManager(&_txn), &version));

    ASSERT_EQUALS(MIN_COMPATIBLE_CONFIG_VERSION, version.getMinCompatibleVersion());
    ASSERT_EQUALS(CURRENT_CONFIG_VERSION, version.getCurrentVersion());
    ASSERT_TRUE(version.getClusterId().isSet());
}

TEST_F(ConfigUpgradeTests, BadVersionUpgrade) {
    //
    // Tests that we can't upgrade from a config version we don't have an upgrade path for
    //

    stopBalancer();

    storeLegacyConfigVersion(1);

    // Default version (not upgradeable)
    ASSERT_EQ(ErrorCodes::IncompatibleShardingMetadata,
              grid.catalogManager(&_txn)->initConfigVersion());
}

}  // namespace
}  // namespace mongo
