/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 */

load("jstests/libs/logv2_helpers.js");

// Checking UUID and index consistency involves mongos being able to do a read from the config
// server, but this test is designed to make mongos time out when reading from the config server.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

(function() {
var st =
    new ShardingTest({shards: 1, configReplSetTestOptions: {settings: {chainingAllowed: false}}});
var testDB = st.s.getDB('test');

assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));

// Ensures that all metadata writes thus far have been replicated to all nodes
st.configRS.awaitReplication();

var configSecondaryList = st.configRS.getSecondaries();
var configSecondaryToKill = configSecondaryList[0];
var delayedConfigSecondary = configSecondaryList[1];

assert.commandWorked(testDB.user.insert({_id: 1}));

delayedConfigSecondary.getDB('admin').adminCommand(
    {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

// Do one metadata write in order to bump the optime on mongos
assert.commandWorked(st.getDB('config').TestConfigColl.insert({TestKey: 'Test value'}));

st.configRS.stopMaster();
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
let msgB = 'Command on database config timed out waiting for read concern to be satisfied';
if (isJsonLogNoConn()) {
    msgB = /Command timed out waiting for read concern to be satisfied.*"db":"config"/;
}

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
    60000,
    300);

// Can't do clean shutdown with this failpoint on.
delayedConfigSecondary.getDB('admin').adminCommand(
    {configureFailPoint: 'rsSyncApplyStop', mode: 'off'});

st.stop();
}());
