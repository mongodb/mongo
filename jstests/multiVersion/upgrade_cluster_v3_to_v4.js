/**
 * Tests upgrading a cluster which has 2.0-style sharded collections as well as 2.2-style sharded
 * collections to be compatible with 2.4.
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )

// BIG OUTER LOOP, RS CLUSTER OR NOT!
for( var test = 0; test < 4; test++ ){

var isRSCluster = (test % 2 == 1);
var isSyncCluster = (test / 2 >= 1);

jsTest.log( "Starting" + ( isRSCluster ? " (replica set)" : "" ) + " cluster" + 
                         ( isSyncCluster ? " (sync)" : "" ) + "..." );

jsTest.log( "Starting 2.0 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.0" },
    configOptions : { binVersion : "2.0" },
    shardOptions : { binVersion : "2.0" },
    
    rsOptions : { binVersion : "2.0" /*, oplogSize : 100, smallfiles : null */ },
    
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
// Make sure 2.4 mongoses won't start in 2.0 cluster
//

jsTest.log("Starting v2.4 mongos in 2.0 cluster...")

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr })
assert.eq(null, mongos);

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongos);

jsTest.log("2.4 mongoses did not start or upgrade in 2.0 cluster (which is correct).")

//
// Add sharded collection in 2.0 cluster
//

/**
 * Creates a sharded collection and splits on both shards, to ensure multi-version clusters
 * have metadata created by mongoses and mongods of different versions.
 */
var createShardedCollection = function(admin, coll) {
    
    printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
    printjson(admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
    printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
    
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } }));    
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }));
    
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -300 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -200 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -100 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 100 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 200 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 300 } }));
    
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : -200 }, to : shards[1]._id }));
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 200 }, to : shards[0]._id }));
}

jsTest.log("Creating new collection in 2.0 cluster...");

var mongos20 = st.s0;

createShardedCollection(mongos20.getDB("admin"), mongos20.getCollection("foo20.bar20"));

st.printShardingStatus();

//
// Upgrade 2.0 cluster to 2.0/2.2
//

jsTest.log("Upgrading 2.0 cluster to 2.0/2.2 cluster...");

st.upgradeCluster(MongoRunner.versionIterator(["2.0","2.2"]));
// Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

//
// Make sure 2.4 mongoses won't start in 2.0/2.2 cluster
//

jsTest.log("Starting v2.4 mongos in 2.0/2.2 cluster....")

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr })
assert.eq(null, mongos);

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongos);

jsTest.log("2.4 mongoses did not start or upgrade in 2.0/2.2 cluster (which is correct).")

//
// Add sharded collections in 2.0/2.2 cluster
//

jsTest.log("Creating new collection in 2.0/2.2 cluster...");

var mongos22 = st.getMongosAtVersion("2.2")
var mongos20 = st.getMongosAtVersion("2.0")

createShardedCollection(mongos20.getDB("admin"), mongos20.getCollection("fooMixed20.barMixed20"));
createShardedCollection(mongos22.getDB("admin"), mongos22.getCollection("fooMixed22.barMixed22"));

st.printShardingStatus();

//
// Upgrade 2.0/2.2 cluster to only 2.2
//

jsTest.log("Upgrading 2.0/2.2 cluster to 2.2 cluster...");

st.upgradeCluster("2.2");
st.restartMongoses();

//
// Make sure 2.4 mongoses will successfully upgrade in 2.4 cluster
//

jsTest.log("Starting v2.4 mongos in 2.2 cluster....")

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr })
assert.eq(null, mongos);

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" })
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

jsTest.log("2.4 mongoses started in 2.2 cluster.")

//
// Add sharded collection in 2.2 cluster
//

jsTest.log("Creating new collection in 2.2 cluster...");

var mongos22 = st.getMongosAtVersion("2.2")

createShardedCollection(mongos22.getDB("admin"), mongos22.getCollection("foo22.bar22"));

st.printShardingStatus();

//
// Verify that all collections have correct epochs in new cluster
//

var config = mongos22.getDB("config")

var collections = config.collections.find().toArray();
var chunks = config.chunks.find().toArray();

for (var i = 0; i < collections.length; i++) {
    
    var collection = collections[i];
    var epoch = collection.lastmodEpoch
    assert(epoch);
    
    for (var j = 0; j < chunks.length; j++) {
        var chunk = chunks[j];
        if (chunk.ns != collection._id) continue;
        assert.eq(chunk.lastmodEpoch, epoch);
    }
}

//
// Verify backup collections are present
//

var collNames = config.getCollectionNames();

var hasBackupColls = false;
var hasBackupChunks = false;

for (var i = 0; i < collNames.length; i++) {
    var collName = collNames[i];
    if (/^collections-backup/.test(collName)) {
        print("Found backup collections " + collName + "...")
        hasBackupColls = true;
    }
    if (/^chunks-backup/.test(collName)) {
        print("Found backup chunks " + collName + "...")
        hasBackupChunks = true;
    }
}

assert(hasBackupColls);
assert(hasBackupChunks);

//
// Verify cluster version is correct
//

var version = config.getMongo().getCollection("config.version").findOne();
printjson(version)

assert.eq(version.version, 3);
assert.eq(version.minCompatibleVersion, 3);
assert.eq(version.currentVersion, 4);
assert(version.clusterId);
assert.eq(version.excluding, undefined);

jsTest.log("DONE!")

st.stop();

} // END OUTER LOOP FOR RS CLUSTER








