/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 * @tags: [
 *   config_shard_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isDisagg} from "jstests/libs/oplog_truncation_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// The following checks involve reading from the config server, but this test is designed to make
// mongos time out when reading from the config server.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

let st = new ShardingTest({
    shards: 1,
    config: 3,
    configReplSetTestOptions: {settings: {chainingAllowed: false}},
    other: {
        mongosOptions: {
            setParameter: {
                "failpoint.disableShardingUptimeReporting": "{mode: 'alwaysOn'}",
                // ShardingTest use a high config command timeout to avoid spurious failures but
                // this test intentionally triggers a timeout, so we restore the default value.
                defaultConfigCommandTimeoutMS: 30000,
            },
        },
    },
});

let testDB = st.s.getDB("test");

assert.commandWorked(testDB.adminCommand({enableSharding: "test"}));
assert.commandWorked(testDB.adminCommand({shardCollection: "test.user", key: {_id: 1}}));

let configSecondaryList = st.configRS.getSecondaries();
let configSecondaryToKill = configSecondaryList[0];
let delayedConfigSecondary = configSecondaryList[1];

assert.commandWorked(testDB.user.insert({_id: 1}));

// Ensures that all metadata writes thus far have been replicated to all nodes
st.configRS.awaitReplication();

// ASC uses the oplog applier, which stopServerReplication() pauses. DSC standbys instead
// replay the disaggregated log through the standby read loop, so pause that loop to keep
// the secondary lagged across the collMod catalog updates.
let standbyReadLoopFailPoint = null;
if (isDisagg(st.configRS.getPrimary())) {
    standbyReadLoopFailPoint = configureFailPoint(delayedConfigSecondary, "hangInStandbyReadLoop");
    assert.commandWorked(
        st.configRS
            .getPrimary()
            .getDB("test")
            .replication_barrier.insert({_id: 1}, {writeConcern: {w: 1}}),
    );
    standbyReadLoopFailPoint.wait();
} else {
    stopServerReplication(delayedConfigSecondary);
}

// Do one metadata write in order to bump the optime on mongos
assert.commandWorked(st.getDB("config").TestConfigColl.insert({TestKey: "Test value"}));

st.configRS.stopPrimary();
MongoRunner.stopMongod(configSecondaryToKill);

// Clears all cached info so mongos will be forced to query from the config.
st.s.adminCommand({flushRouterConfig: 1});

print("Attempting read on a sharded collection...");
let exception = assert.throws(function () {
    testDB.user.find({}).maxTimeMS(15000).itcount();
});

assert(ErrorCodes.isExceededTimeLimitError(exception.code));

let msgAA = 'command config.$cmd command: find { find: "databases"';
let msgAB = "errCode:" + ErrorCodes.ClientDisconnect;
let msgB = /Command timed out waiting for read concern to be satisfied.*"db":"config"/;

assert.soon(
    function () {
        let logMessages = assert.commandWorked(
            delayedConfigSecondary.adminCommand({getLog: "global"}),
        ).log;
        for (let i = 0; i < logMessages.length; i++) {
            if (
                (logMessages[i].indexOf(msgAA) != -1 && logMessages[i].indexOf(msgAB) != -1) ||
                logMessages[i].search(msgB) != -1
            ) {
                return true;
            }
        }
        return false;
    },
    "Did not see any log entries containing the following message: " +
        msgAA +
        " ... " +
        msgAB +
        " or " +
        msgB,
    180000,
    300,
);

// Can't do clean shutdown with this failpoint on.
if (standbyReadLoopFailPoint !== null) {
    standbyReadLoopFailPoint.off();
} else {
    restartServerReplication(delayedConfigSecondary);
}
st.stop();
