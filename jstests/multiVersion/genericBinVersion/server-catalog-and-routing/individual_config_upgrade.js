/**
 * Individual config server upgrade test.
 */

import {testPerformIndividualConfigUpgrade} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const databaseName = jsTestName();
const collectionName = "coll";

/**
 * Helper function to assert that the cluster maintains quorum and has a writable primary.
 * Retries with fixed 1-second intervals to handle temporary unavailability during config upgrades.
 *
 * @param {Mongo} mongos - The mongos connection to test
 * @param {string} message - Error message to display if assertion fails
 * @param {number} timeoutMs - Maximum time to wait in milliseconds (default: 30000)
 */
function assertQuorumMaintained(mongos, message, timeoutMs = 30000) {
    assert.soon(
        () => {
            try {
                let hello = mongos.getDB("admin")._helloOrLegacyHello();
                if (!hello.isWritablePrimary && !hello.ismaster) {
                    jsTest.log.info("Waiting for writable primary, current state:", hello);
                    return false;
                }
                return true;
            } catch (e) {
                jsTest.log.error("Failed to contact primary: " + e);
                return false;
            }
        },
        message,
        timeoutMs,
        1000,
    );
}

function testIndividualConfigUpgrade() {
    testPerformIndividualConfigUpgrade({
        setupFn: (mongos, st) => {
            assert.commandWorked(mongos.getDB(databaseName)[collectionName].insert({initial: true}));

            // Verify all 3 config servers are up
            assert.eq(st.configRS.nodes.length, 3);
            jsTest.log.info("Initial config primary: " + st.configRS.getPrimary());

            // The individual config server upgrade test stops one config server at a time, so we need to
            // reduce election timeout to speed up elections of config primary during test.
            const config = st.configRS.getReplSetConfigFromNode();
            config.version++;
            config.settings = config.settings || {};
            config.settings = {electionTimeoutMillis: 1000};
            assert.commandWorked(
                st.configRS.getPrimary().adminCommand({
                    replSetReconfig: config,
                    force: true,
                }),
            );
            st.configRS.awaitReplication();
            st.configRS.waitForConfigReplication(st.configRS.getPrimary());
        },

        beforeRestart: (mongos) => {
            // Verify cluster is fully operational before upgrades
            assert.commandWorked(mongos.getDB(databaseName)[collectionName].insert({beforeUpgrade: true}));
            assert.commandWorked(mongos.adminCommand({listDatabases: 1}));
        },

        afterFirstConfigUpgraded: (mongos) => {
            jsTest.log.info("Testing cluster operations with 1 upgraded config server (mixed versions)");

            // Verify primary is still elected
            assertQuorumMaintained(mongos, "Failed to maintain quorum after first config upgrade");

            assert.commandWorked(mongos.getDB(databaseName)[collectionName].insert({afterFirstConfig: true}));
            assert.eq(mongos.getDB(databaseName)[collectionName].count(), 3);
        },

        afterSecondConfigUpgraded: (mongos) => {
            jsTest.log.info("Testing cluster operations with 2 upgraded config servers (majority new version)");

            // Verify quorum maintained on new version
            assertQuorumMaintained(mongos, "Failed to maintain quorum after second config upgrade");

            assert.commandWorked(mongos.getDB(databaseName)[collectionName].insert({afterSecondConfig: true}));
            assert.eq(mongos.getDB(databaseName)[collectionName].count(), 4);
        },

        afterAllConfigsUpgraded: (mongos) => {
            jsTest.log.info("Testing cluster operations with all config servers upgraded");

            // Verify cluster is fully operational
            assert.commandWorked(mongos.getDB(databaseName)[collectionName].insert({allConfigsUpgraded: true}));
            assert.eq(mongos.getDB(databaseName)[collectionName].count(), 5);

            // Verify all documents inserted during upgrade are present
            assert.eq(mongos.getDB(databaseName)[collectionName].count({initial: true}), 1);
            assert.eq(mongos.getDB(databaseName)[collectionName].count({beforeUpgrade: true}), 1);
            assert.eq(mongos.getDB(databaseName)[collectionName].count({afterFirstConfig: true}), 1);
            assert.eq(mongos.getDB(databaseName)[collectionName].count({afterSecondConfig: true}), 1);
            assert.eq(mongos.getDB(databaseName)[collectionName].count({allConfigsUpgraded: true}), 1);
        },
    });
}

// Run the tests
jsTest.log.info("Running individual config server upgrade");
testIndividualConfigUpgrade();

jsTest.log.info("All individual config server upgrade completed successfully");
