/** Test TTL collections with replication
 *  Part 1: Initiate replica set. Insert some docs and create a TTL index. Check that both primary
 *          and secondary get userFlags=1 (indicating that usePowerOf2Sizes is on),
 *          and check that the correct # of docs age out.
 *  Part 2: Add a new member to the set.  Check it also gets userFlags=1 and correct # of docs.
 *  Part 3: Change the TTL expireAfterSeconds field and check successful propogation to secondary.
 */

var rt = new ReplSetTest( { name : "ttl_repl" , nodes: 2 } );

/******** Part 1 ***************/

// setup set
var nodes = rt.startSet();
rt.initiate();
var master = rt.getMaster();
rt.awaitSecondaryNodes();
var slave1 = rt.liveNodes.slaves[0];

// shortcuts
var masterdb = master.getDB( 'd' );
var slave1db = slave1.getDB( 'd' );
var mastercol = masterdb[ 'c' ];
var slave1col = slave1db[ 'c' ];

// create new collection. insert 24 docs, aged at one-hour intervalss
mastercol.drop();
now = (new Date()).getTime();
for ( i=0; i<24; i++ )
    mastercol.insert( { x : new Date( now - ( 3600 * 1000 * i ) ) } );
masterdb.getLastError();
rt.awaitReplication();
assert.eq( 24 , mastercol.count() , "docs not inserted on primary" );
assert.eq( 24 , slave1col.count() , "docs not inserted on secondary" );

// before TTL index created, check that userFlags are 0
print("Initial Stats:")
print("Master:");
printjson( mastercol.stats() );
print("Slave1:");
printjson( slave1col.stats() );

assert.eq( 0 , mastercol.stats().userFlags , "userFlags not 0 on primary");
assert.eq( 0 , slave1col.stats().userFlags , "userFlags not 0 on secondary");

// create TTL index, wait for TTL monitor to kick in, then check that
// userFlags get set to 1, and correct number of docs age out
mastercol.ensureIndex( { x : 1 } , { expireAfterSeconds : 20000 } );
masterdb.getLastError();
rt.awaitReplication();

sleep(70*1000); // TTL monitor runs every 60 seconds, so wait 70

print("Stats after waiting for TTL Monitor:")
print("Master:");
printjson( mastercol.stats() );
print("Slave1:");
printjson( slave1col.stats() );

assert.eq( 1 , mastercol.stats().userFlags , "userFlags not 1 on primary" );
assert.eq( 1 , slave1col.stats().userFlags , "userFlags not 1 on secondary" );
assert.eq( 6 , mastercol.count() , "docs not deleted on primary" );
assert.eq( 6 , slave1col.count() , "docs not deleted on secondary" );


/******** Part 2 ***************/

// add a new secondary, wait for it to fully join
var slave = rt.add();
rt.reInitiate();
rt.awaitSecondaryNodes();

var slave2col = slave.getDB( 'd' )[ 'c' ];

// check that its userFlags are also 1, and it has right number of docs
print("New Slave stats:");
printjson( slave2col.stats() );

assert.eq( 1 , slave2col.stats().userFlags , "userFlags not 1 on new secondary");
assert.eq( 6 , slave2col.count() , "wrong number of docs on new secondary");


/******* Part 3 *****************/
//Check that the collMod command successfully updates the expireAfterSeconds field
masterdb.runCommand( { collMod : "c",
                       index : { keyPattern : {x : 1}, expireAfterSeconds : 10000} } );
rt.awaitReplication();

var newTTLindex = { "key": { "x" : 1 } , "ns": "d.c" , "expireAfterSeconds" : 10000 };
assert.eq( 1, masterdb.system.indexes.find( newTTLindex ).count(),
           "primary index didn't get updated");
assert.eq( 1, slave1db.system.indexes.find( newTTLindex ).count(),
           "secondary index didn't get updated");

// finish up
rt.stopSet();
