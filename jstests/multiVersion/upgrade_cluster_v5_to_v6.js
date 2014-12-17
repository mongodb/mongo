/**
 * Tests upgrading a cluster which has 2.6 mongos.
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )

/**
 * @param isRSCluster {bool} use replica set shards.
 * @param isSyncCluster {bool} use 3 config servers.
 */
var runTest = function(isRSCluster, isSyncCluster) {

jsTest.log( "Starting" + ( isRSCluster ? " (replica set)" : "" ) + " cluster" + 
                         ( isSyncCluster ? " (sync)" : "" ) + "..." );

jsTest.log( "Starting 2.4 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.4" },
    configOptions : { binVersion : "2.4" },
    shardOptions : { binVersion : "2.4" },
    
    rsOptions : { binVersion : "2.4" /*, oplogSize : 100, smallfiles : null */ },
    
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
// Make sure 2.8 mongoses won't start in 2.4 cluster
//

jsTest.log("Starting v2.8 mongos in 2.4 cluster...");

var mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongos);

jsTest.log("2.8 mongoses did not start or upgrade in 2.4 cluster (which is correct).");

//
// Upgrade 2.4 cluster to 2.4/2.6
//

var configDB = st.s.getDB('config');
var clusterID = configDB.getCollection('version').findOne().clusterId;

jsTest.log("Upgrading 2.4 cluster to 2.4/2.6 cluster...");

// upgrade config to v4 (This is a required to make 2.6 mongos startable).
mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

var version = configDB.getCollection('version').findOne();
printjson(version);

assert.eq(version.minCompatibleVersion, 4);
assert.eq(version.currentVersion, 5);
assert.eq(clusterID, version.clusterId); // clusterId shouldn't change
assert.eq(version.excluding, undefined);

st.upgradeCluster(MongoRunner.versionIterator(["2.4","2.6"]));
// Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

//
// Make sure 2.8 mongoses won't start in 2.4/2.6 cluster
//

jsTest.log("Starting v2.8 mongos in 2.4/2.6 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr });
assert.eq(null, mongos);

jsTest.log("2.8 mongoses did not start or upgrade in 2.4/2.6 cluster (which is correct).");

//
// Upgrade 2.4/2.6 cluster to only 2.6
//

jsTest.log("Upgrading 2.4/2.6 cluster to 2.6 cluster...");

st.upgradeCluster("2.6");
st.restartMongoses();

//
// Make sure 2.8 mongoses will successfully upgrade in 2.6 cluster
//

jsTest.log("Starting v2.8 mongos in 2.6 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

jsTest.log("2.8 mongoses started in 2.6 cluster.");

//
// Verify cluster version is correct
//

configDB = st.s.getDB('config'); // Get a new db since a restart happened.
version = configDB.getCollection('version').findOne();
printjson(version);

assert.eq(version.minCompatibleVersion, 5);
assert.eq(version.currentVersion, 6);
assert.eq(clusterID, version.clusterId); // clusterId shouldn't change
assert.eq(version.excluding, undefined);

// Make sure that you can't run 2.4 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr });
assert.eq(null, mongos);

// Make sure that you can run 2.6 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

// Make sure that you can run 2.8 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.8", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

jsTest.log("DONE!")

st.stop();

};

runTest(false, false);
runTest(false, true);
runTest(true, false);
runTest(true, true);
