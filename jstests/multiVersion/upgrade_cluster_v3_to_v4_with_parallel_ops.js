/**
 * Tests upgrading a config db which has different types of sharded collection data from v3 to v4
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )
load( './jstests/libs/test_background_ops.js' )

jsTest.log( "Starting mixed 2.2/2.0 cluster..." );

var options = {
    
    mongosOptions : { binVersion : MongoRunner.versionIterator(["2.0","2.2"]) },
    configOptions : { binVersion : MongoRunner.versionIterator(["2.0","2.2"]) },
    shardOptions : { binVersion : MongoRunner.versionIterator(["2.0","2.2"]) },
    
    separateConfig : true,
    sync : true
}

//
// Create a basic mixed sharded cluster with extra mongoses to do work while we upgrade
//

var st = new ShardingTest({ shards : 2, mongos : 3, other : options });

var mongos = st.s0
var parallelMongoses = st._mongos.concat([]).splice(1);
var config = mongos.getDB("config")
var admin = mongos.getDB("admin")

var shards = config.shards.find().toArray();
var configConnStr = st._configDB;
var originalVersion = config.getMongo().getCollection("config.version").findOne();

//
// Shard a collection for each of our extra mongoses and distribute chunks to v2.0 and v2.2 shards
//

st.stopBalancer();

var shardedColls = [];
for (var i = 0; i < parallelMongoses.length; i++) {
    
    var parallelMongos = parallelMongoses[i];
    
    var coll = parallelMongos.getCollection("foo" + i + ".bar" + i);
    var admin = parallelMongos.getDB("admin");
    
    printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
    printjson(admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
    printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
    
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } }));    
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }));
    
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -0.3 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -0.2 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : -0.1 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 0.1 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 0.2 } }));
    printjson(admin.runCommand({ split : coll + "", middle : { _id : 0.3 } }));
    
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : -0.2 }, to : shards[1]._id }));
    printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 0.2 }, to : shards[0]._id }));
    
    shardedColls.push(coll);
}

st.startBalancer();

st.printShardingStatus();

//
// Upgrade cluster to v2.2
//

jsTest.log("Upgrading cluster to 2.2...")

st.upgradeCluster("2.2");
//Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

jsTest.log("Cluster upgraded...")

var mongos = st.s0
var parallelMongoses = st._mongos.concat([]).splice(1);
var config = mongos.getDB("config");
var admin = mongos.getDB("admin");

function splitAndMove( mongosURL, shards, ns ){
    
    var coll = null;
    
    // Make sure we can eventually connect to the mongos
    assert.soon( function(){
        try{ 
            print("Waiting for connect to " + mongosURL + "...");
            coll = new Mongo(mongosURL).getCollection(ns + "");
            return true;
        }
        catch (e) {
            printjson(e);
            return false;
        }
    })
    
    var splitCount = 0;
    var moveCount = 0;
    
    jsTest.log("Starting splits and moves to " + ns + "...")
    
    while (!isFinished()) {
        try {
            // Split 75% of the time
            var isSplit = Random.rand() < 0.75;
            var key = (Random.rand() * 2.0) - 1.0;
            var shard = shards[parseInt(Random.rand() * shards.length)]
            
            var admin = coll.getMongo().getDB("admin");
            var result = null;
            
            if (isSplit) {
                result = admin.runCommand({ split : coll + "", 
                                            middle : { _id : key } });
                splitCount++;
                result["isSplit"] = true;
            }
            else {
                result = admin.runCommand({ moveChunk : coll + "",
                                            find : key,
                                            to : shards[shard]._id })
                moveCount++;
                result["isMove"] = true;
            }
            
            if (result.ok != 0) printjson(result);
        }
        catch (e) {
            sleep(1);
            printjson(e);
        }
    }
    
    jsTest.log("Finished splits and moves to " + ns + "...")
    return { splitCount : splitCount, moveCount : moveCount };
}

//
// Start split and move operations in the 2.2 cluster
//

jsTest.log("Starting split and move operations...")

var staticMongod = MongoRunner.runMongod({})
printjson( staticMongod )

var joinSplitAndMoves = [];
for (var i = 0; i < parallelMongoses.length; i++) {
    joinSplitAndMoves.push( 
        startParallelOps( staticMongod, // The connection where the test info is passed and stored
                          splitAndMove,
                          [ parallelMongoses[i].host, shards, shardedColls[i] + "" ] )
    );
}

jsTest.log("Sleeping for metadata operations to start...")

sleep(10 * 1000);

printShardingStatus(config, true);

//
// Do a config upgrade while the split and move operations are active
//

jsTest.log("Upgrading config db from v3 to v4...");

// Just stop the balancer, but don't wait for it to stop, to simulate race conds
st.setBalancer(false);
printjson(config.settings.find().toArray());

var startTime = new Date();

// Make sure up
var mongosNew = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" })
assert.neq(null, mongosNew);
MongoRunner.stopMongos(mongosNew);

var endTime = new Date();

jsTest.log( "Config db upgrade took " + ((endTime - startTime) / 1000) + " secs" );

//
// Stop the split and move operations
//

for (var i = 0; i < parallelMongoses.length; i++) {
    joinSplitAndMoves[i]();
}

printShardingStatus(config, true)

//
// Make sure our cluster was successfully upgraded with epochs
//

var checkUpgraded = function() {

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
    // Verify that all collections have correct epochs in new cluster
    //
    var collections = config.collections.find().toArray();
    
    var chunks = config.chunks.find().toArray();
    for (var i = 0; i < collections.length; i++) {
        
        var collection = collections[i];
        if (collection.dropped) continue;
        
        var epoch = collection.lastmodEpoch
        assert(epoch);
        
        for (var j = 0; j < chunks.length; j++) {
            var chunk = chunks[j];
            if (chunk.ns != collection._id) continue;
            assert.eq(chunk.lastmodEpoch, epoch);
        }
    }
    
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
}

checkUpgraded();

jsTest.log("DONE!")

st.stop();








