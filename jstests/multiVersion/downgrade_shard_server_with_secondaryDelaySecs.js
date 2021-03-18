/**
 * Tests the downgrade procedure for a shardsvr in binary version 4.9 and FCV 4.4 with the
 * 'secondaryDelaySecs' field:
 * 1. Start up shard server processes on MDB 4.9 / FCV 4.4.
 * 2. rs.init() with secondaryDelaySecs.
 * 3. Downgrade shard binary to MDB 4.4 / FCV 4.4 (server won't automatically change
 *    existing 'secondaryDelaySecs' fields to 'slaveDelay' since that occurs only when
 *    downgrading the FCV). The shard server will fail to start up.
 * 4. Restart shard with MDB 4.9 / FCV 4.4.
 * 5. Reconfig to change 'secondaryDelaySecs' to 'slaveDelay'.
 * 6. Downgrade shard binary to MDB 4.4 / FCV 4.4. The shard server should successfully
 *    start up.
 *
 * TODO SERVER-55128: Remove this test once 5.0 becomes 'lastLTS'.
 */

(function() {
"use strict";

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

function checkForSecondaryDelaySecs(primaryDB) {
    let config = primaryDB.runCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[0].secondaryDelaySecs, 0, config);
    assert(!config.members[0].hasOwnProperty('slaveDelay'), config);
}

// A 'latest' binary replica set started as a shard server defaults to 'lastLTSFCV'. Allow
// the use of 'secondaryDelaySecs' before the shard server is added to a cluster.
let shardServer = new ReplSetTest({
    name: "shardServer",
    nodes: [{binVersion: "latest", rsConfig: {secondaryDelaySecs: 0}}],
    nodeOptions: {shardsvr: ""},
    useHostName: true
});

shardServer.startSet();
shardServer.initiate();
let shardServerPrimaryAdminDB = shardServer.getPrimary().getDB("admin");
checkFCV(shardServerPrimaryAdminDB, lastLTSFCV);
// Even though shardServer is in the 'lastLTSFCV', its config should still have the
// 'secondaryDelaySecs' field.
checkForSecondaryDelaySecs(shardServerPrimaryAdminDB);

shardServer.stopSet(null /* signal */, true /* forRestart */);

// Downgrade the shard server replica set binary. This should fail because the shard
// server's config still has 'secondaryDelaySecs'.
jsTest.log("Restarting shardServer with a lastLTS binVersion.");
try {
    shardServer.startSet({restart: true, binVersion: "last-lts"});
    // We expect startSet to fail, so make sure the test fails if startSet unexpectedly
    // succeeds.
    assert(false);
} catch (e) {
    assert(e.message.includes("Failed to connect"));
}

// Restart with the 'latest' binary.
shardServer.startSet({restart: true, binVersion: "latest"});
shardServerPrimaryAdminDB = shardServer.getPrimary().getDB("admin");
checkFCV(shardServerPrimaryAdminDB, lastLTSFCV);

// Reconfig to change 'secondaryDelaySecs' to 'slaveDelay'.
let config = shardServerPrimaryAdminDB.runCommand({replSetGetConfig: 1}).config;
delete config.members[0].secondaryDelaySecs;
config.members[0].slaveDelay = 0;
config.version++;

assert.commandWorked(shardServerPrimaryAdminDB.runCommand({replSetReconfig: config}));

// Now, downgrading the shard server binaries to lastLTS binary should succeed.
shardServer.stopSet(null /* signal */, true /* forRestart */);
shardServer.startSet({restart: true, binVersion: "last-lts"});

// Check that the featureCompatiblityVersion is set to lastLTSFCV.
shardServerPrimaryAdminDB = shardServer.getPrimary().getDB("admin");
checkFCV(shardServerPrimaryAdminDB, lastLTSFCV);

shardServer.stopSet();
})();
