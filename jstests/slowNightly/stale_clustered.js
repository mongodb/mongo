// this tests that slaveOk'd queries in sharded setups get correctly routed when
// a slave goes into RECOVERING state, and don't break

function prt(s) {
    print("\nstale_clustered.js " + s);
    print();
}

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


var rsA = shardTest._rs[0].test
var rsB = shardTest._rs[1].test

rsA.getMaster().getDB( "test_a" ).dummy.insert( { x : 1 } )
rsB.getMaster().getDB( "test_b" ).dummy.insert( { x : 1 } )

rsA.awaitReplication()
rsB.awaitReplication()

prt("1: initial insert")

coll.save({ _id : -1, a : "a", date : new Date() })
coll.save({ _id : 1, b : "b", date : new Date() })

prt("2: shard collection")

shardTest.shardGo( coll, /* shardBy */ { _id : 1 }, /* splitAt */ { _id : 0 } )

prt("3: test normal and slaveOk queries")

// Make shardA and rsA the same
var shardA = shardTest.getShard(  coll, { _id : -1 } )
var shardAColl = shardA.getCollection( "" + coll )
var shardB = shardTest.getShard(  coll, { _id : 1 } )

if( shardA.name == rsB.getURL() ){
    var swap = rsB
    rsB = rsA
    rsA = swap
}

rsA.awaitReplication()
rsB.awaitReplication()

// Because of async migration cleanup, we need to wait for this condition to be true
assert.soon( function(){ return coll.find().itcount() == collSOk.find().itcount() } )
assert.eq( shardAColl.find().itcount(), 1 )
assert.eq( shardAColl.findOne()._id, -1 )

prt("5: overflow oplog");

var secs = rsA.getSecondaries()
var goodSec = secs[0]
var badSec = secs[1]

rsA.overflow( badSec )

prt("6: stop non-overflowed secondary")

rsA.stop( goodSec, undefined, true )

prt("7: check our regular and slaveOk query")

assert.eq( coll.find().itcount(), collSOk.find().itcount() )

prt("8: restart both our secondaries clean")

rsA.restart( rsA.getSecondaries(), { remember : true, startClean : true }, undefined, 5 * 60 * 1000 )

prt("9: wait for recovery")

rsA.waitForState( rsA.getSecondaries(), rsA.SECONDARY, 5 * 60 * 1000 )

prt("10: check our regular and slaveOk query")

// We need to make sure our nodes are considered accessible from mongos - otherwise we fail
// See SERVER-7274
ReplSetTest.awaitRSClientHosts(coll.getMongo(), rsA.nodes, { ok : true })
ReplSetTest.awaitRSClientHosts(coll.getMongo(), rsB.nodes, { ok : true })

// We need to make sure at least one secondary is accessible from mongos - otherwise we fail
// See SERVER-7699
ReplSetTest.awaitRSClientHosts(collSOk.getMongo(), [rsA.getSecondaries()[0]], 
                               { secondary : true, ok : true });
ReplSetTest.awaitRSClientHosts(collSOk.getMongo(), [rsB.getSecondaries()[0]], 
                               { secondary : true, ok : true });

prt("SlaveOK Query...")
var sOKCount = collSOk.find().itcount();

var collCount = null
try{
    prt("Normal query...")
    collCount = coll.find().itcount();
}
catch(e){
    printjson(e);
    
    // there may have been a stepdown caused by step 8, so we run this twice in a row. The first time
    // can error out
    
    prt("Error may have been caused by stepdown, try again.")
    collCount = coll.find().itcount();
}

assert.eq( collCount, sOKCount );

prt("DONE\n\n\n");

//shardTest.stop()

