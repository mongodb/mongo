// this tests that slaveOk'd queries in sharded setups get correctly routed when
// a slave goes into RECOVERING state, and don't break


var shardTest = new ShardingTest( name = "clusteredstale" ,
                                  numShards = 2 ,
                                  verboseLevel = 0 ,
                                  numMongos = 2 ,
                                  otherParams = { rs : true } )//, 
                                                  //rs0 : { logpath : "$path/mongod.log" }, 
                                                  //rs1 : { logpath : "$path/mongod.log" } } );

shardTest.setBalancer( false )

var mongos = shardTest.s0
var mongosSOK = shardTest.s1
mongosSOK.setSlaveOk()

var admin = mongos.getDB("admin")
var config = mongos.getDB("config")

var dbase = mongos.getDB("test")
var coll = dbase.getCollection("foo")
var dbaseSOk = mongosSOK.getDB( "" + dbase )
var collSOk = mongosSOK.getCollection( "" + coll )

print("1: initial insert")

coll.save({ _id : -1, a : "a", date : new Date() })
coll.save({ _id : 1, b : "b", date : new Date() })

print("2: shard collection")

shardTest.shardGo( coll, /* shardBy */ { _id : 1 }, /* splitAt */ { _id : 0 } )

print("3: test normal and slaveOk queries")

// Make shardA and rsA the same
var shardA = shardTest.getShard(  coll, { _id : -1 } )
var shardAColl = shardA.getCollection( "" + coll )
var shardB = shardTest.getShard(  coll, { _id : 1 } )

var rsA = shardTest._rs[0].test
var rsB = shardTest._rs[1].test

if( shardA.name == rsB.getURL() ){
    var swap = rsB
    rsB = rsA
    rsA = swap
}

rsA.awaitReplication()
rsB.awaitReplication()

assert.eq( coll.find().itcount(), collSOk.find().itcount() )
assert.eq( shardAColl.find().itcount(), 1 )
assert.eq( shardAColl.findOne()._id, -1 )

print("5: overflow oplog");

var secs = rsA.getSecondaries()
var goodSec = secs[0]
var badSec = secs[1]

rsA.overflow( badSec )

print("6: stop non-overflowed secondary")

rsA.stop( goodSec, undefined, true )

print("7: check our regular and slaveok query")

assert.eq( coll.find().itcount(), collSOk.find().itcount() )

print("8: restart both our secondaries clean")

rsA.restart( rsA.getSecondaries(), { remember : true, startClean : true }, undefined, 5 * 60 * 1000 )

print("9: wait for recovery")

rsA.waitForState( rsA.getSecondaries(), rsA.SECONDARY, 5 * 60 * 1000 )

print("10: check our regular and slaveok query")

assert.eq( coll.find().itcount(), collSOk.find().itcount() )




shardTest.stop()

