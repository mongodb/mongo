/**
 * Tests upgrading a cluster which has 2.4 mongos.
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

jsTest.log( "Starting 2.2 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.2" },
    configOptions : { binVersion : "2.2" },
    shardOptions : { binVersion : "2.2" },
    
    rsOptions : { binVersion : "2.2" /*, oplogSize : 100, smallfiles : null */ },
    
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
// Make sure 2.6 mongoses won't start in 2.2 cluster
//

jsTest.log("Starting v2.6 mongos in 2.2 cluster...");

var mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongos);

jsTest.log("2.6 mongoses did not start or upgrade in 2.2 cluster (which is correct).");

//
// Upgrade 2.2 cluster to 2.2/2.4
//

jsTest.log("Upgrading 2.2 cluster to 2.2/2.4 cluster...");

// upgrade config to v4 (This is a required to make 2.4 mongos startable).
mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos.port);

st.upgradeCluster(MongoRunner.versionIterator(["2.2","2.4"]));
// Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

//
// Make sure 2.4 mongoses won't start in 2.2/2.4 cluster
//

jsTest.log("Starting v2.6 mongos in 2.2/2.4 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

jsTest.log("2.6 mongoses did not start or upgrade in 2.2/2.4 cluster (which is correct).");

//
// Upgrade 2.2/2.4 cluster to only 2.4
//

jsTest.log("Upgrading 2.2/2.4 cluster to 2.4 cluster...");

st.upgradeCluster("2.4");
st.restartMongoses();

var configDB = st.s.getDB('config');
var clusterID = configDB.getCollection('version').findOne().clusterId;

//
// Make sure 2.6 mongoses will successfully upgrade in 2.4 cluster
//

jsTest.log("Starting v2.6 mongos in 2.4 cluster....");

mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.eq(null, mongos);

mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr, upgrade : "" });
assert.eq(null, mongos);

jsTest.log("2.6 mongoses started in 2.4 cluster.");

//
// Verify cluster version is correct
//

var version = configDB.getCollection('version').findOne();
printjson(version);

assert.eq(version.version, 4);
assert.eq(version.minCompatibleVersion, 4);
assert.eq(version.currentVersion, 5);
assert.eq(clusterID, version.clusterId); // clusterId shouldn't change
assert.eq(version.excluding, undefined);

// Make sure that you can't run 2.2 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.2", configdb : configConnStr });
assert.eq(null, mongos);

// Make sure that you can run 2.4 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

// Make sure that you can run 2.6 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

jsTest.log("DONE!")

st.stop();

};

runTest(false, false);
runTest(false, true);
runTest(true, false);
runTest(true, true);
