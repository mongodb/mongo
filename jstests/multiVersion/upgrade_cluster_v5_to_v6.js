/**
 * Tests upgrading a cluster which has 3.0 mongos.
 */

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

(function() {

/**
 * @param isRSCluster {bool} use replica set shards.
 */
var runTest = function(isRSCluster) {
"use strict";

jsTest.log( "Starting" + ( isRSCluster ? " (replica set)" : "" ) + " cluster" + "..." );

jsTest.log( "Starting 2.6 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.6" },
    configOptions : { binVersion : "2.6" },
    shardOptions : { binVersion : "2.6" },
    
    rsOptions : { binVersion : "2.6" /*, oplogSize : 100, smallfiles : null */ },

    sync: true, // Old clusters can't use replsets for config servers
    rs : isRSCluster
}

// This function attempts to connect to mongos for 5 seconds and if it can NOT
// connect then it returns true. This is needed to ensure that mongos can not start.
var cantStartMongos = function(options) {
    var mongos = MongoRunner.runMongos(options);
    var attempts = 5;
    while (attempts--) {
        try {
            mongosConn = new Mongo(mongos.host);
            // do not expect it can start
            return false;
        }
        catch (e) {
            print("Waiting for connect...");
            sleep(1000);
        }
    }
    MongoRunner.stopMongos(mongos);
    return true;

}

var st = new ShardingTest({ shards : 2, mongos : 2, other : options });

var shards = st.s0.getDB("config").shards.find().toArray();
var configConnStr = st._configDB;

//
// Make sure 3.2 mongoses won't start in 2.6 cluster
//

jsTest.log("Starting v3.2 mongos in 2.6 cluster...");

// 3.2. mongos will start but will be waiting for config server to connect so need to check if 
// shell can connect to it. This is expected because currently mongos 3.2 can only connect to 
// mongod 3.2, hence in 2.6 cluster it will be waiting indefinitely.
assert(cantStartMongos({ binVersion : "3.2", configdb : configConnStr, waitForConnect : false}));
assert(cantStartMongos({ binVersion : "3.2", configdb : configConnStr, upgrade: "", 
                        waitForConnect : false}));

jsTest.log("3.2 mongoses did not start or upgrade in 2.6 cluster (which is correct).");

//
// Upgrade 2.6 cluster to 2.6/3.0
//
var configDB = st.s.getDB('config');
var clusterID = configDB.getCollection('version').findOne().clusterId;

jsTest.log("Upgrading 2.6 cluster to 2.6/3.0 cluster...");

// upgrade config to v4 (This is a required to make 3.0 mongos startable).
var mongos = MongoRunner.runMongos({ binVersion : "3.0", configdb : configConnStr, upgrade : "" });
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

jsTest.log("Upgraded to 2.6/3.0 cluster")
//
// Upgrade 2.6/3.0 cluster to only 3.0
//

jsTest.log("Upgrading 2.6/3.0 cluster to 3.0 cluster...");

st.upgradeCluster("3.0");
st.restartMongoses();

jsTest.log("Upgraded to 3.0 cluster")
//
// Upgrade 3.0 cluster to only 3.2
//

// The two step upgrade process is needed because upgradeCluster upgrades mongos first and 
// mongos can not connect to mongod 3.0 or less. 
// TODO(SERVER-21577): Remove this extra step and let the upgradeCluster function
// handle it once upgradeCluster has been fixed to do the upgrade in the right order.
jsTest.log("Upgrading 3.0 cluster to 3.2 cluster...");
st.upgradeCluster("3.2", { upgradeMongos: false });
st.restartMongoses();

jsTest.log("Upgraded shards and configs to 3.2 cluster")

st.upgradeCluster("3.2", { upgradeConfigs: false, upgradeShards: false });
st.restartMongoses();

jsTest.log("Upgraded to 3.2 cluster")
//
// Verify cluster version is correct
//

// Make sure that you can run 2.6 mongos
mongos = MongoRunner.runMongos({ binVersion : "2.6", configdb : configConnStr });
assert.neq(null, mongos);

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

runTest(false);
runTest(true);

})();
