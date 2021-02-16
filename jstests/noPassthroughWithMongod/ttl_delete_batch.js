/** Test TTL collections with 'ttlDeleteBatch'
 *  Part 1: Initiate replica set. Insert some docs. set 'ttlDeleteBatch' to 1.
 *  Part 2: Create a TTL index.
 *          Check that the correct counts of docs on both primary and secondary nodes.
 *  Part 3: set 'ttlDeleteBatch' to a big value
 *          Check that all the docs will be deleted
 *  @tags: [requires_replication]
*/

load("jstests/replsets/rslib.js")

var rt = new ReplSetTest({name: "ttl_delete_batch", nodes: 2});

/******** Part 1 ***************/

// setup set
var nodes = rt.startSet();
rt.initiate();
var master = rt.getPrimary();
rt.awaitSecondaryNodes();
var slave1 = rt._slaves[0];

// shortcuts
var masterdb = master.getDB('d');
var slave1db = slave1.getDB('d');
var mastercol = masterdb['c'];
var slave1col = slave1db['c'];

mastercol.drop();
masterdb.createCollection(mastercol.getName(), {usePowerOf2Sizes: false});

// create new collection, insert 100 docs
var bulk = mastercol.initializeUnorderedBulkOp();
for (i = 0; i < 100; i++) {
    bulk.insert({a: new Date(), b: i});
}
assert.writeOK(bulk.execute());
rt.awaitReplication();
assert.eq(100, mastercol.count(), "docs not inserted on primary");
assert.eq(100, slave1col.count(), "docs not inserted on secondary");

print("Initial Stats:");
print("Master:");
printjson(mastercol.stats());
print("Slave1:");
printjson(slave1col.stats());

// set 'ttlDeleteBatch' to 1
assert.commandWorked(mastercol.getDB().adminCommand({setParameter: 1, ttlDeleteBatch: 1}));

/******** Part 2 ***************/

// Create TTL index, wait for TTL monitor to kick in, then check that
// the correct number of docs on both primary and secondary
assert.commandWorked(mastercol.ensureIndex({a: 1}, {expireAfterSeconds: 60}));
rt.awaitReplication();

print("wait 120 seconds for at least 1 ttl operation");
sleep(120 * 1000);   // TTL monitor runs every 60 seconds

print("Stats after waiting for TTL Monitor:");
print("Master:");
printjson(mastercol.stats());
print("Slave1:");
printjson(slave1col.stats());

// only 1 or 2 docs will be deleted, as 'ttlDeleteBatch' was set to 1
assert.gte(mastercol.count(), 98, "incorrect doc count on primary");
assert.gte(mastercol.count(), 98, "incorrect doc count on primary");
assert.lte(slave1col.count(), 99, "incorrect doc count on secondary");
assert.lte(slave1col.count(), 99, "incorrect doc count on secondary");

/******* Part 3 *****************/

// set 'ttlDeleteBatch' to 10000
assert.commandWorked(mastercol.getDB().adminCommand({setParameter: 1, ttlDeleteBatch: 10000}));

print("wait 70 seconds for at least 1 ttl operation");
sleep(70 * 1000);

// make sure that all the docs are deleted
assert.eq(mastercol.count(), 0, "some docs are not deleted on primary");
assert.eq(slave1col.count(), 0, "some docs are not deleted on secondary");

// finish up
rt.stopSet();
