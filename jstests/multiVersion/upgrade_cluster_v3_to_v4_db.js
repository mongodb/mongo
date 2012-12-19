/**
 * Tests upgrading a config db which has different types of sharded collection data from v3 to v4
 */

load( './jstests/multiVersion/libs/multi_rs.js' )
load( './jstests/multiVersion/libs/multi_cluster.js' )

jsTest.log( "Starting 2.2 cluster..." );

var options = {
    
    mongosOptions : { binVersion : "2.2" },
    configOptions : { binVersion : "2.2" },
    shardOptions : { binVersion : "2.2" },
    
    separateConfig : true,
    sync : false
}

var st = new ShardingTest({ shards : 1, mongos : 1, other : options });

var mongos = st.s0
var config = mongos.getDB("config")
var admin = mongos.getDB("admin")
var shards = config.shards.find().toArray();
var configConnStr = st._configDB;
var originalVersion = config.getMongo().getCollection("config.version").findOne();

st.printShardingStatus();

var resetBackupDBs = function() {
    
    var configConn = new Mongo(configConnStr);
    var databases = configConn.getDBs().databases;
    
    //
    // Drop all new backup databases
    //
    
    for (var i = 0; i < databases.length; i++) {
        var dbName = databases[i].name + "";
        if (!/^config$|^admin$|^local$/.test(dbName)) {
            print("Dropping " + dbName + "...")
            configConn.getDB(dbName).dropDatabase();
        }
    }
}

var resetVersion = function() {    
    config.getMongo().getCollection("config.version").update({ _id : 1 }, originalVersion, true);
    assert.eq(null, config.getLastError());
}

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
    // Verify cluster version is correct
    //

    var version = config.getMongo().getCollection("config.version").findOne();
    printjson(version)

    assert.eq(version.version, 3);
    assert.eq(version.minCompatibleVersion, 3);
    assert.eq(version.currentVersion, 4);
    assert(version.clusterId);
    assert.eq(version.excluding.length, 0)
    
}

//
// Default config upgrade
//

jsTest.log("Upgrading empty config server from v3 to v4...");

// Make sure up
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.neq(null, mongosNew);
MongoRunner.stopMongos(mongosNew);
checkUpgraded();
resetVersion();
resetBackupDBs();


//
// Invalid config.version upgrade
//

jsTest.log("Clearing config.version collection...")

config.getMongo().getCollection("config.version").remove({})
assert.eq(null, config.getLastError());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongosNew);
resetVersion();
resetBackupDBs();


//
// Bad config.version upgrade
//

jsTest.log("Adding bad config.version data...")

config.getMongo().getCollection("config.version").update({ _id : 1 }, { $unset : { version : 1 } });
assert.eq(null, config.getLastError());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongosNew);
resetVersion();
resetBackupDBs();


//
// Invalid config.collections upgrade
//

jsTest.log("Adding bad sharded collection data...")

var coll = mongos.getCollection("foo.bar");

printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } })); 

config.collections.update({ _id : coll + "" }, { $set : { lastmodEpoch : ObjectId() }});
assert.eq(null, config.getLastError());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongosNew);
resetBackupDBs();


//
// Dropped collection upgrade
//

jsTest.log("Adding bad (dropped) sharded collection data...")

config.collections.update({ _id : coll + "" }, { $set : { dropped : true }});

// Make sure up
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.neq(null, mongosNew);
MongoRunner.stopMongos(mongosNew);
checkUpgraded();
resetVersion();
resetBackupDBs();


//
// Invalid chunks collection upgrade
//

jsTest.log("Adding bad sharded chunks data...")

var coll = mongos.getCollection("foo2.bar2");

printjson(admin.runCommand({ enableSharding : coll.getDB() + "" }));
printjson(admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }));
printjson(admin.runCommand({ split : coll + "", middle : { _id : 0 } })); 

config.chunks.update({ ns : coll + "" }, { $set : { lastmodEpoch : ObjectId() }});
assert.eq(null, config.getLastError());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr, upgrade : "" })
assert.eq(null, mongosNew);
resetBackupDBs();

jsTest.log("DONE!")

st.stop();








