/**
 * Tests upgrading a cluster which has 3.0 mongos.
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )

/**
 * @param isRSCluster {bool} use replica set shards.
 * @param isSyncCluster {bool} use 3 config servers.
 */
var runTest = function(isRSCluster, isSyncCluster) {
"use strict";

jsTest.log( "Starting" + ( isRSCluster ? " (replica set)" : "" ) + " cluster" + 
                         ( isSyncCluster ? " (sync)" : "" ) + "..." );

jsTest.log( "Starting 2.6 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.6" },
    configOptions : { binVersion : "2.6" },
    shardOptions : { binVersion : "2.6" },
    
    rsOptions : { binVersion : "2.6" /*, oplogSize : 100, smallfiles : null */ },
    
    separateConfig : true,
    sync : isSyncCluster,
    rs : isRSCluster
}

var st = new ShardingTest({ shards : 2, mongos : 2, other : options });

// Just stop balancer, to simulate race conds
st.setBalancer(false);

var shards = st.s0.getDB("config").shards.find().toArray();
var configConnStr = st._configDB;

//
// Make sure 3.2 mongoses won't start in 2.6 cluster
//

jsTest.log("Starting v3.2 mongos in 2.6 cluster...");

var mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

jsTest.log("3.2 mongoses did not start or upgrade in 2.6 cluster (which is correct).");

//
// Upgrade 2.6 cluster to 2.6/3.0
//

var configDB = st.s.getDB('config');
var clusterID = configDB.getCollection('version').findOne().clusterId;

jsTest.log("Upgrading 2.6 cluster to 2.6/3.0 cluster...");

// upgrade config to v4 (This is a required to make 3.0 mongos startable).
mongos = MongoRunner.runMongos({ binVersion : "3.0", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

var version = configDB.getCollection('version').findOne();
printjson(version);

assert.eq(version.minCompatibleVersion, 5);
assert.eq(version.currentVersion, 6);
assert.eq(clusterID, version.clusterId); // clusterId shouldn't change
assert.eq(version.excluding, undefined);

st.upgradeCluster(MongoRunner.versionIterator(["2.6","3.0"]));
// Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

//
// Make sure 3.2 mongoses won't start in 2.6/3.0 cluster
//

jsTest.log("Starting v3.2 mongos in 2.6/3.0 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr });
assert.eq(null, mongos);

jsTest.log("3.2 mongoses did not start or upgrade in 2.6/3.0 cluster (which is correct).");

//
// Upgrade 2.6/3.0 cluster to only 3.0
//

jsTest.log("Upgrading 2.6/3.0 cluster to 3.0 cluster...");

st.upgradeCluster("3.0");
st.restartMongoses();

//
// Make sure 3.2 mongoses will successfully upgrade in 3.0 cluster
//

jsTest.log("Starting v3.2 mongos in 3.0 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

jsTest.log("3.2 mongoses started in 3.0 cluster.");

//
// Verify cluster version is correct
//

configDB = st.s.getDB('config'); // Get a new db since a restart happened.
version = configDB.getCollection('version').findOne();
printjson(version);

assert.eq(version.minCompatibleVersion, 6);
assert.eq(version.currentVersion, 7);
assert.eq(clusterID, version.clusterId); // clusterId shouldn't change
assert.eq(version.excluding, undefined);

// Make sure that you can't run 2.6 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.eq(null, mongos);

// Make sure that you can run 3.0 mongos
mongos = MongoRunner.runMongos({ binVersion : "3.0", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

// Make sure that you can run 3.2 mongos
mongos = MongoRunner.runMongos({ binVersion : "3.2", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

jsTest.log("DONE!")

st.stop();

};

runTest(false, false);
runTest(false, true);
runTest(true, false);
runTest(true, true);
