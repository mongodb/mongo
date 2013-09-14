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
 */

#include "mongo/dbtests/config_server_fixture.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/config_upgrade.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_shard.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_config_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version.h"

namespace mongo {

    /**
     * Specialization of the config server fixture with helpers for the tests below.
     */
    class ConfigUpgradeFixture: public ConfigServerFixture {
    public:

        void stopBalancer() {
            // Note: The balancer key is needed in the update portion, for some reason related to 
            // DBDirectClient
            client().update(SettingsType::ConfigNS, 
                            BSON(SettingsType::key("balancer")),
                            BSON(SettingsType::key("balancer") << SettingsType::balancerStopped(true)),
                            true, false);
        }

        /**
         * Stores a legacy { version : X } config server entry
         */
        void storeLegacyConfigVersion(int version) {

            if (version == 0) return;

            if (version == 1) {
                ShardType shard;
                shard.setName("test");
                shard.setHost("$dummy:10000");
                client().insert(ShardType::ConfigNS, shard.toBSON());
                return;
            }

            client().insert(VersionType::ConfigNS, BSON("_id" << 1 << "version" << version));
        }

        /**
         * Stores a newer { version, minVersion, currentVersion, clusterId } config server entry
         */
        void storeConfigVersion(const VersionType& versionInfo) {
            client().insert(VersionType::ConfigNS, versionInfo.toBSON());
        }

        /**
         * Stores a newer { version, minVersion, currentVersion, clusterId } config server entry.
         *
         * @return clusterId
         */
        OID storeConfigVersion(int configVersion) {

            if (configVersion < UpgradeHistory_MandatoryEpochVersion) {
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

            for (int i = 0; i < numShards; i++) {
                ShardType shard;
                shard.setName(OID::gen().toString());
                shard.setHost((string) (str::stream() << "$dummyShard:" << (i + 1) << "0000"));

                client().insert(ShardType::ConfigNS, shard.toBSON());
            }

            for (int i = 0; i < numPings; i++) {

                MongosType ping;
                ping.setName((string) (str::stream() << "$dummyMongos:" << (i + 1) << "0000"));
                ping.setPing(jsTime());
                ping.setMongoVersion(versionString);
                ping.setConfigVersion(CURRENT_CONFIG_VERSION);

                if (i % 2 == 0) {
                    ping.setPing(ping.getPing() - 10 * 60 * 1000);
                }

                client().insert(MongosType::ConfigNS, ping.toBSON());
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
        Status status = getConfigVersion(configSvr(), &oldVersion);
        ASSERT(status.isOK());

        ASSERT_EQUALS(oldVersion.getMinCompatibleVersion(), 0);
        ASSERT_EQUALS(oldVersion.getCurrentVersion(), 0);
    }

    TEST_F(ConfigUpgradeTests, LegacyVersion) {

        //
        // Tests detection of legacy config versions
        //

        for (int i = 1; i <= 3; i++) {

            clearVersion();
            clearShards(); // B/C version 1 is weird, needs other collections
            storeLegacyConfigVersion(i);

            // Legacy versions 2->3
            VersionType oldVersion;
            Status status = getConfigVersion(configSvr(), &oldVersion);
            ASSERT(status.isOK());

            ASSERT_EQUALS(oldVersion.getMinCompatibleVersion(), i);
            ASSERT_EQUALS(oldVersion.getCurrentVersion(), i);
        }
    }

    TEST_F(ConfigUpgradeTests, ClusterIDVersion) {

        //
        // Tests detection of newer config versions
        //

        VersionType newVersion;
        newVersion.setMinCompatibleVersion(UpgradeHistory_NoEpochVersion);
        newVersion.setCurrentVersion(UpgradeHistory_MandatoryEpochVersion);
        storeConfigVersion(newVersion);

        newVersion.clear();

        // Current Version w/o clusterId (invalid!)
        Status status = getConfigVersion(configSvr(), &newVersion);
        ASSERT(!status.isOK());

        newVersion.clear();

        OID clusterId = OID::gen();
        newVersion.setClusterId(clusterId);
        newVersion.setMinCompatibleVersion(UpgradeHistory_NoEpochVersion);
        newVersion.setCurrentVersion(UpgradeHistory_MandatoryEpochVersion);

        clearVersion();
        storeConfigVersion(newVersion);

        newVersion.clear();

        // Current version w/ clusterId (valid!)
        status = getConfigVersion(configSvr(), &newVersion);
        ASSERT(status.isOK());

        ASSERT_EQUALS(newVersion.getMinCompatibleVersion(), UpgradeHistory_NoEpochVersion);
        ASSERT_EQUALS(newVersion.getCurrentVersion(), UpgradeHistory_MandatoryEpochVersion);
        ASSERT_EQUALS(newVersion.getClusterId(), clusterId);
    }

    TEST_F(ConfigUpgradeTests, InitialUpgrade) {

        //
        // Tests initializing the config server to the initial version
        //

        // Empty version
        VersionType versionOld;
        VersionType version;
        string errMsg;
        bool result = checkAndUpgradeConfigVersion(configSvr(),
                                                   false,
                                                   &versionOld,
                                                   &version,
                                                   &errMsg);

        ASSERT(result);
        ASSERT_EQUALS(versionOld.getCurrentVersion(), 0);
        ASSERT_EQUALS(version.getMinCompatibleVersion(), MIN_COMPATIBLE_CONFIG_VERSION);
        ASSERT_EQUALS(version.getCurrentVersion(), CURRENT_CONFIG_VERSION);
        ASSERT_NOT_EQUALS(version.getClusterId(), OID());
    }

    TEST_F(ConfigUpgradeTests, BadVersionUpgrade) {

        //
        // Tests that we can't upgrade from a config version we don't have an upgrade path for
        //

        stopBalancer();

        storeLegacyConfigVersion(1);

        // Default version (not upgradeable)
        VersionType versionOld;
        VersionType version;
        string errMsg;
        bool result = checkAndUpgradeConfigVersion(configSvr(),
                                                   false,
                                                   &versionOld,
                                                   &version,
                                                   &errMsg);

        ASSERT(!result);
    }

    TEST_F(ConfigUpgradeTests, CheckMongoVersion) {

        //
        // Tests basic detection of existing mongos and mongod versions from mongos ping
        // and shard info.  Fuller tests require conns to multiple version mongos processes, not
        // done here.
        //

        storeShardsAndPings(5, 10); // 5 shards, 10 pings

        // Our version is >= 2.2, so this works
        Status status = checkClusterMongoVersions(configSvr(), "2.2");
        ASSERT(status.isOK());

        // Our version is < 9.9, so this doesn't work (until we hit v99.99)
        status = checkClusterMongoVersions(configSvr(), "99.99");
        ASSERT(status.code() == ErrorCodes::RemoteValidationError);
    }

} // end namespace
