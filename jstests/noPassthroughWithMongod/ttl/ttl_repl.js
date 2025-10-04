/**
 * Test TTL collections with replication
 *  Part 1: Initiate replica set. Insert some docs and create a TTL index.
 *          Check that the correct # of docs age out.
 *  Part 2: Add a new member to the set. Check that it also gets the correct # of docs.
 *  Part 3: Change the TTL expireAfterSeconds field and check successful propogation to secondary.
 *  @tags: [requires_replication]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconfig} from "jstests/replsets/rslib.js";

let rt = new ReplSetTest({name: "ttl_repl", nodes: 2});

/******** Part 1 ***************/

// setup set
let nodes = rt.startSet();
rt.initiate();
let primary = rt.getPrimary();
rt.awaitSecondaryNodes();
let secondary1 = rt.getSecondary();

// shortcuts
let primarydb = primary.getDB("d");
let secondary1db = secondary1.getDB("d");
let primarycol = primarydb["c"];
let secondary1col = secondary1db["c"];

primarycol.drop();
primarydb.createCollection(primarycol.getName());

// create new collection. insert 24 docs, aged at one-hour intervalss
let now = new Date().getTime();
let bulk = primarycol.initializeUnorderedBulkOp();
for (let i = 0; i < 24; i++) {
    bulk.insert({x: new Date(now - 3600 * 1000 * i)});
}
assert.commandWorked(bulk.execute());
rt.awaitReplication();
assert.eq(24, primarycol.count(), "docs not inserted on primary");
assert.eq(24, secondary1col.count(), "docs not inserted on secondary");

print("Initial Stats:");
print("Primary:");
printjson(primarycol.stats());
print("Secondary1:");
printjson(secondary1col.stats());

// create TTL index, wait for TTL monitor to kick in, then check that
// the correct number of docs age out
let initialExpireAfterSeconds = 20000;
assert.commandWorked(primarycol.createIndex({x: 1}, {expireAfterSeconds: initialExpireAfterSeconds}));
rt.awaitReplication();

sleep(70 * 1000); // TTL monitor runs every 60 seconds, so wait 70

print("Stats after waiting for TTL Monitor:");
print("Primary:");
printjson(primarycol.stats());
print("Secondary1:");
printjson(secondary1col.stats());

assert.eq(6, primarycol.count(), "docs not deleted on primary");
assert.eq(6, secondary1col.count(), "docs not deleted on secondary");

/******** Part 2 ***************/

// add a new secondary, wait for it to fully join
let secondary = rt.add();
let config = rt.getReplSetConfig();
config.version = rt.getReplSetConfigFromNode().version + 1;
reconfig(rt, config);

let secondary2col = secondary.getDB("d")["c"];

// check that the new secondary has the correct number of docs
print("New Secondary stats:");
printjson(secondary2col.stats());

assert.eq(6, secondary2col.count(), "wrong number of docs on new secondary");

/******* Part 3 *****************/
// Check that the collMod command successfully updates the expireAfterSeconds field
primarydb.runCommand({collMod: "c", index: {keyPattern: {x: 1}, expireAfterSeconds: 10000}});
rt.awaitReplication();

function getTTLTime(theCollection, theKey) {
    let indexes = theCollection.getIndexes();
    for (let i = 0; i < indexes.length; i++) {
        if (friendlyEqual(theKey, indexes[i].key)) return indexes[i].expireAfterSeconds;
    }
    throw "not found";
}

printjson(primarydb.c.getIndexes());
assert.eq(10000, getTTLTime(primarydb.c, {x: 1}));
assert.eq(10000, getTTLTime(secondary1db.c, {x: 1}));

// Verify the format of TTL collMod oplog entry. The old expiration time should be saved,
// and index key patterns should be normalized to index names.
let primaryOplog = primary.getDB("local").oplog.rs.find().sort({$natural: 1}).toArray();
let collModEntry = primaryOplog.find((op) => op.o.collMod);

assert(collModEntry, "collMod entry was not present in the oplog.");
assert.eq(initialExpireAfterSeconds, collModEntry.o2["indexOptions_old"]["expireAfterSeconds"]);
assert.eq("x_1", collModEntry.o["index"]["name"]);

// finish up
rt.stopSet();
