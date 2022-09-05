/**
 * Test that mongos times out when the config server replica set only contains nodes that
 * are behind the majority opTime.
 */

load("jstests/libs/write_concern_util.js");

// Checking UUID and index consistency involves mongos being able to do a read from the config
// server, but this test is designed to make mongos time out when reading from the config server.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;

(function() {

/**
 * On the config server the lastApplied optime can go past the atClusterTime timestamp due to pings
 * made on collection config.mongos or config.lockping by the distributed lock pinger thread and
 * sharding uptime reporter thread. This can cause the insert into test.TestConfigColl to time out.
 * To prevent this, disable the pinger threads to prevent them reaching out to the config server.
 */
const failpointParams = {
    // TODO SERVER-68551: Remove once 7.0 becomes last-lts
    setParameter: {"failpoint.disableReplSetDistLockManager": "{mode: 'alwaysOn'}"}
};

var st = new ShardingTest({
    shards: 1,
    configReplSetTestOptions: {settings: {chainingAllowed: false}},
    other: {
        configOptions: failpointParams,
        rsOptions: failpointParams,
        mongosOptions:
            {setParameter: {["failpoint.disableShardingUptimeReporting"]: "{mode: 'alwaysOn'}"}}
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
    60000,
    300);

// Can't do clean shutdown with this failpoint on.
restartServerReplication(delayedConfigSecondary);
st.stop();
}());
