/** Simple test of sharding TTL collections.
 *  - Creates a new collection with a TTL index
 *  - Shards it, and moves one chunk containing half the docs to another shard.
 *  - Checks that both shards have TTL index, and docs get deleted on both shards.
 *  - Run the collMod command to update the expireAfterSeconds field. Check that more docs get
 *    deleted.
 */

// start up a new sharded cluster
var s = new ShardingTest({shards: 2, mongos: 1});

var dbname = "testDB";
var coll = "ttl_sharded";
var ns = dbname + "." + coll;
t = s.getDB(dbname).getCollection(coll);

// enable sharding of the collection. Only 1 chunk initially
s.adminCommand({enablesharding: dbname});
s.ensurePrimaryShard(dbname, 'shard0001');
s.adminCommand({shardcollection: ns, key: {_id: 1}});

// insert 24 docs, with timestamps at one hour intervals
var now = (new Date()).getTime();
var bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < 24; i++) {
    var past = new Date(now - (3600 * 1000 * i));
    bulk.insert({_id: i, x: past});
}
assert.writeOK(bulk.execute());
assert.eq(t.count(), 24, "initial docs not inserted");

// create the TTL index which delete anything older than ~5.5 hours
t.ensureIndex({x: 1}, {expireAfterSeconds: 20000});

// split chunk in half by _id, and move one chunk to the other shard
s.adminCommand({split: ns, middle: {_id: 12}});
s.adminCommand({moveChunk: ns, find: {_id: 0}, to: s.getOther(s.getPrimaryShard(dbname)).name});

// Check that all expired documents are deleted.
assert.soon(
    function() {
        return t.count() === 6 && t.find({x: {$lt: new Date(now - 20000000)}}).count() === 0;
    },
    "TTL index did not successfully delete expired documents, all documents: " +
        tojson(t.find().toArray()),
    70 * 1000);

// now lets check things explicily on each shard
var shard0 = s._connections[0].getDB(dbname);
var shard1 = s._connections[1].getDB(dbname);

print("Shard 0 coll stats:");
printjson(shard0.getCollection(coll).stats());
print("Shard 1 coll stats:");
printjson(shard1.getCollection(coll).stats());

function getTTLTime(theCollection, theKey) {
    var indexes = theCollection.getIndexes();
    for (var i = 0; i < indexes.length; i++) {
        if (friendlyEqual(theKey, indexes[i].key))
            return indexes[i].expireAfterSeconds;
    }
    throw "not found";
}

// Check that TTL index (with expireAfterSeconds field) appears on both shards
assert.eq(20000, getTTLTime(shard0.getCollection(coll), {x: 1}));
assert.eq(20000, getTTLTime(shard1.getCollection(coll), {x: 1}));

// Check that the collMod command successfully updates the expireAfterSeconds field
s.getDB(dbname).runCommand({collMod: coll, index: {keyPattern: {x: 1}, expireAfterSeconds: 10000}});
assert.eq(10000, getTTLTime(shard0.getCollection(coll), {x: 1}));
assert.eq(10000, getTTLTime(shard1.getCollection(coll), {x: 1}));

// Check that all expired documents are deleted.
assert.soon(
    function() {
        return t.count() === 3 && t.find({x: {$lt: new Date(now - 10000000)}}).count() === 0;
    },
    "new expireAfterSeconds did not successfully delete expired documents, all documents: " +
        tojson(t.find().toArray()),
    70 * 1000);

s.stop();
