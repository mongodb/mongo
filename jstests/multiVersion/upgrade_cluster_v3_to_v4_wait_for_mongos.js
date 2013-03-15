/**
 * Tests upgrading a cluster where there was a recently active mongos process.
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
};

var st = new ShardingTest({ shards : 1, mongos : 1, other : options });

// Turn balancer off, don't wait
st.setBalancer(false);

var mongos = st.s0;

jsTest.log( "Starting v2.0 mongos..." );

var mongos20 = MongoRunner.runMongos({ binVersion : "2.0", configdb : st._configDB })

jsTest.log( "Waiting for 2.0 ping document..." );

var hasPing = function() {
  return mongos.getCollection("config.mongos").findOne({ _id : RegExp(":" + mongos20.port + "$") }) != null;
}

assert.soon( hasPing );

jsTest.log( "Stopping 2.0 mongos..." );

MongoRunner.stopMongos(mongos20);

jsTest.log( "Upgrade should be unsuccessful..." );

// Make sure down
var mongosNew = MongoRunner.runMongos({ binVersion : "2.4", configdb : st._configDB, upgrade : "" })
assert.eq(null, mongosNew);

jsTest.log("Resetting time to zero...");

printjson(mongos.getCollection("config.mongos").findOne({ _id : RegExp(":" + mongos20.port + "$") }));

mongos.getCollection("config.mongos").update({ _id : RegExp(":" + mongos20.port + "$") },
                                             { $set : { ping : new Date(0) } });
assert.eq(null, mongos.getDB("config").getLastError());

printjson(mongos.getCollection("config.mongos").findOne({ _id : RegExp(":" + mongos20.port + "$") }));

jsTest.log("Trying to restart mongos...");

// Make sure up
var mongosNew = MongoRunner.runMongos({ binVersion : "2.4", configdb : st._configDB, upgrade : "" })
assert.neq(null, mongosNew);

jsTest.log("Mongos started!");

MongoRunner.stopMongos(mongosNew);

jsTest.log("DONE!")

st.stop();



