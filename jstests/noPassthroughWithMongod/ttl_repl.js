/** Test TTL collections with replication
 *  Part 1: Initiate replica set. Insert some docs and create a TTL index.
 *          Check that the correct # of docs age out.
 *  Part 2: Add a new member to the set. Check that it also gets the correct # of docs.
 *  Part 3: Change the TTL expireAfterSeconds field and check successful propogation to secondary.
 */

load("jstests/replsets/rslib.js");

var rt = new ReplSetTest({name: "ttl_repl", nodes: 2});

/******** Part 1 ***************/

// setup set
var nodes = rt.startSet();
rt.initiate();
var master = rt.getPrimary();
rt.awaitSecondaryNodes();
var slave1 = rt.liveNodes.slaves[0];

// shortcuts
var masterdb = master.getDB('d');
var slave1db = slave1.getDB('d');
var mastercol = masterdb['c'];
var slave1col = slave1db['c'];

// turn off usePowerOf2Sizes as this tests the flag is set automatically
mastercol.drop();
masterdb.createCollection(mastercol.getName(), {usePowerOf2Sizes: false});

// create new collection. insert 24 docs, aged at one-hour intervalss
now = (new Date()).getTime();
var bulk = mastercol.initializeUnorderedBulkOp();
for (i = 0; i < 24; i++) {
    bulk.insert({x: new Date(now - (3600 * 1000 * i))});
}
assert.writeOK(bulk.execute());
rt.awaitReplication();
assert.eq(24, mastercol.count(), "docs not inserted on primary");
assert.eq(24, slave1col.count(), "docs not inserted on secondary");

print("Initial Stats:");
print("Master:");
printjson(mastercol.stats());
print("Slave1:");
printjson(slave1col.stats());

// create TTL index, wait for TTL monitor to kick in, then check that
// the correct number of docs age out
assert.commandWorked(mastercol.ensureIndex({x: 1}, {expireAfterSeconds: 20000}));
rt.awaitReplication();

sleep(70 * 1000);  // TTL monitor runs every 60 seconds, so wait 70

print("Stats after waiting for TTL Monitor:");
print("Master:");
printjson(mastercol.stats());
print("Slave1:");
printjson(slave1col.stats());

assert.eq(6, mastercol.count(), "docs not deleted on primary");
assert.eq(6, slave1col.count(), "docs not deleted on secondary");

/******** Part 2 ***************/

// add a new secondary, wait for it to fully join
var slave = rt.add();
var config = rt.getReplSetConfig();
config.version = 2;
reconfig(rt, config);

var slave2col = slave.getDB('d')['c'];

// check that the new secondary has the correct number of docs
print("New Slave stats:");
printjson(slave2col.stats());

assert.eq(6, slave2col.count(), "wrong number of docs on new secondary");

/******* Part 3 *****************/
// Check that the collMod command successfully updates the expireAfterSeconds field
masterdb.runCommand({collMod: "c", index: {keyPattern: {x: 1}, expireAfterSeconds: 10000}});
rt.awaitReplication();

function getTTLTime(theCollection, theKey) {
    var indexes = theCollection.getIndexes();
    for (var i = 0; i < indexes.length; i++) {
        if (friendlyEqual(theKey, indexes[i].key))
            return indexes[i].expireAfterSeconds;
    }
    throw "not found";
}

printjson(masterdb.c.getIndexes());
assert.eq(10000, getTTLTime(masterdb.c, {x: 1}));
assert.eq(10000, getTTLTime(slave1db.c, {x: 1}));

// finish up
rt.stopSet();
