//
// Test checks whether or not config version excludes prevent mongos startup
//

jsTest.log( "Starting cluster..." );

var options = {
               
    mongosOptions : { },
    configOptions : {  },
    shardOptions : { },
    
    separateConfig : true,
    sync : false
}

var st = new ShardingTest({ shards : 2, mongos : 3, other : options });

var mongos = st.s0
var parallelMongoses = st._mongos.concat([]).splice(1);
var config = mongos.getDB("config")
var configVersion = config.getMongo().getCollection("config.version");
var admin = mongos.getDB("admin")

var configConnStr = st._configDB;

// Make sure adding an excluded version actually prevents mongos from starting

jsTest.log("Testing excluded mongos versions...")

configVersion.update({ _id : 1 }, { $addToSet : { excluding : "2.3" } });
configVersion.update({ _id : 1 }, { $addToSet : { excluding : "2.4" } });
printjson(configVersion.findOne());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr })
assert.eq(null, mongosNew);

jsTest.log("Testing excluded ranges...")

configVersion.update({ _id : 1 }, { $unset : { excluding : 1 } });
configVersion.update({ _id : 1 }, { $addToSet : { excluding : ["2.3", "2.4"] } });
printjson(configVersion.findOne());

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr })
assert.eq(null, mongosNew);

jsTest.log("Testing empty excluded...")

configVersion.update({ _id : 1 }, { $unset : { excluding : 1 } });
printjson(configVersion.findOne());

// Make sure up
var mongosNew = MongoRunner.runMongos({ binVersion : "latest", configdb : configConnStr })
assert.neq(null, mongosNew);
MongoRunner.stopMongos(mongosNew);

jsTest.log("DONE!")

st.stop();
