/**
 * Tests upgrading a cluster to config version 4 with epochs and ensures 2.2 mongod will write the
 * correct epochs on the next move/split.
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )

jsTest.log( "Starting 2.0 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.0" },
    configOptions : { binVersion : "2.0" },
    shardOptions : { binVersion : "2.0" },
    
    separateConfig : true,
    sync : false
}

var st = new ShardingTest({ shards : 3, mongos : 2, other : options });

// Stop balancer, otherwise the balancer lock can get wedged for 15 mins after our upgrades
// on bad mongos shutdowns.  
st.stopBalancer();

var shards = st.s0.getDB("config").shards.find().toArray();
var configConnStr = st._configDB;

//
// Add sharded collection in 2.0 cluster
//

jsTest.log("Creating new collection in 2.0 cluster...");

var mongos20 = st.s0;
var coll = mongos20.getCollection("foo.bar");
var admin = mongos20.getDB("admin");

printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));

st.printShardingStatus();

//
// Upgrade 2.0 cluster to 2.0/2.2
//

jsTest.log("Upgrading 2.0 cluster to 2.2 cluster...");

st.upgradeCluster("2.2");
// Restart of mongos here is unfortunately necessary, connection pooling otherwise causes problems
st.restartMongoses();

var mongos22A = st.s0;
var mongos22B = st.s1;

jsTest.log("Performing metadata operations without epochs...");

var coll = mongos22A.getCollection("foo.bar");
var admin = mongos22A.getDB("admin");
var config = mongos22A.getDB("config");

//Split collection into several parts
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 1 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 2 } }));

//Put one part on each shard
printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 1 }, to : shards[1]._id }));
printjson(admin.runCommand({ moveChunk : coll + "", find : { _id : 2 }, to : shards[2]._id }));

//Split the first chunk into two parts, so all shards will have chunks after one migrate
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0.5 } }));

// Make sure mongos22B is up-to-date with the correct version
mongos22B.getCollection("foo.bar").findOne();

printjson(config.chunks.find().toArray());

//
// Upgrade cluster to new version
//

jsTest.log("Starting v2.4 mongos in 2.2 cluster....")

var mongos = MongoRunner.runMongos({ binVersion : "2.4", configdb : configConnStr, upgrade : "" })
assert.neq(null, mongos);
MongoRunner.stopMongos(mongos);

jsTest.log("2.4 mongos upgraded cluster.")

//
// Do more metadata operations with 2.2 mongoses without updating versions
//

jsTest.log("Doing more metadata operations with un-refreshed 2.2 mongoses and ds");

// Do a move operation
printjson(mongos22A.getDB("admin").runCommand({ moveChunk : coll + "", 
                                                find : { _id : 0 }, to : shards[1]._id }));

// Do a split operation
printjson(mongos22B.getDB("admin").runCommand({ split : coll + "", 
                                                middle : { _id : 3 } }));

printjson(config.chunks.find().toArray());

//
// Check that this didn't screw up our upgrade
//

jsTest.log("Was upgraded?")

var checkUpgraded = function() {
    
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








