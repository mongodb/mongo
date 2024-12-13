/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 * @tags: [
 *   config_shard_incompatible,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// The following checks involve reading from the config server, but this test is designed to make
// mongos time out when reading from the config server.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

var st = new ShardingTest({
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
            }
        }
    }
});

var testDB = st.s.getDB('test');

assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));

var configSecondaryList = st.configRS.getSecondaries();
var configSecondaryToKill = configSecondaryList[0];
var delayedConfigSecondary = configSecondaryList[1];

assert.commandWorked(testDB.user.insert({_id: 1}));

// Ensures that all metadata writes thus far have been replicated to all nodes
st.configRS.awaitReplication();

stopServerReplication(delayedConfigSecondary);

// Do one metadata write in order to bump the optime on mongos
assert.commandWorked(st.getDB('config').TestConfigColl.insert({TestKey: 'Test value'}));

st.configRS.stopPrimary();
MongoRunner.stopMongod(configSecondaryToKill);

// Clears all cached info so mongos will be forced to query from the config.
st.s.adminCommand({flushRouterConfig: 1});

print('Attempting read on a sharded collection...');
var exception = assert.throws(function() {
    testDB.user.find({}).maxTimeMS(15000).itcount();
});

assert(ErrorCodes.isExceededTimeLimitError(exception.code));

let msgAA = 'command config.$cmd command: find { find: "databases"';
let msgAB = 'errCode:' + ErrorCodes.ClientDisconnect;
let msgB = /Command timed out waiting for read concern to be satisfied.*"db":"config"/;

assert.soon(
    function() {
        var logMessages =
            assert.commandWorked(delayedConfigSecondary.adminCommand({getLog: 'global'})).log;
        for (var i = 0; i < logMessages.length; i++) {
            if ((logMessages[i].indexOf(msgAA) != -1 && logMessages[i].indexOf(msgAB) != -1) ||
                logMessages[i].search(msgB) != -1) {
                return true;
            }
        }
        return false;
    },
    'Did not see any log entries containing the following message: ' + msgAA + ' ... ' + msgAB +
        ' or ' + msgB,
    180000,
    300);

// Can't do clean shutdown with this failpoint on.
restartServerReplication(delayedConfigSecondary);
st.stop();
