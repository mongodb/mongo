// A mongos participating in a cluster that has been upgraded both a binary and FCV version above it
// should crash.
//
// This kind of scenario can happen when a user forgets to upgrade the mongos binary and then calls
// setFCV(upgrade), leaving the still downgraded mongos unable to communicate. Rather than the
// mongos logging incompatible server version errors endlessly, we've chosen to crash it.

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");

const lastStable = "last-stable";

let st = new ShardingTest({mongos: 1, shards: 1});
const ns = "testDB.testColl";
let mongosAdminDB = st.s.getDB("admin");

// Assert that a mongos using the 'last-stable' binary version will crash when connecting to a
// cluster running on the 'latest' binary version with the 'latest' FCV.
let lastStableMongos =
    MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: lastStable});

assert(!lastStableMongos);

// Assert that a mongos using the 'last-stable' binary version will successfully connect to a
// cluster running on the 'latest' binary version with the 'last-stable' FCV.
assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
lastStableMongos = MongoRunner.runMongos({configdb: st.configRS.getURL(), binVersion: lastStable});
assert.neq(null,
           lastStableMongos,
           "mongos was unable to start up with binary version=" + lastStable +
               " and connect to FCV=" + lastStableFCV + " cluster");

// Ensure that the 'lastStable' binary mongos can perform reads and writes to the shards in the
// cluster.
assert.commandWorked(lastStableMongos.getDB("test").foo.insert({x: 1}));
let foundDoc = lastStableMongos.getDB("test").foo.findOne({x: 1});
assert.neq(null, foundDoc);
assert.eq(1, foundDoc.x, tojson(foundDoc));

// Assert that the 'lastStable' binary mongos will crash after the cluster is upgraded to
// 'latestFCV'.
assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
let error = assert.throws(function() {
    lastStableMongos.getDB("test").foo.insert({x: 1});
});
assert(isNetworkError(error));
assert(!lastStableMongos.conn);

st.stop();
})();
