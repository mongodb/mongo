// All collections created during a map-reduce should be replicated to secondaries unless they are
// in the "local" or "admin" databases. Any collection outside of "local" that does not get
// replicated is a potential problem for workloads with transactions (see SERVER-35365 and
// SERVER-35282).
//
// We verify this requirement by running a map-reduce, examining the logs to find the names of
// all collections created, and checking the oplog for entries logging the creation of each of those
// collections.

(function() {
"use strict";

const name = "mr_nonrepl_coll_in_local_db";
const replSet = new ReplSetTest({name: name, nodes: 2});
replSet.startSet();
replSet.initiate();

const dbName = name;
const collName = "test";

const primary = replSet.getPrimary();
const primaryDB = primary.getDB(dbName);
const coll = primaryDB[collName];

// Insert 1000 documents in the "test" collection.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1000; i++) {
    const array = Array.from({lengthToInsert: 10000}, _ => Math.floor(Math.random() * 100));
    bulk.insert({arr: array});
}
assert.commandWorked(bulk.execute());

// Run a simple map-reduce.
const result = coll.mapReduce(
    function map() {
        return this.arr.forEach(element => emit(element, 1));
    },
    function reduce(key, values) {
        return Array.sum(values);
    },
    {query: {arr: {$exists: true}}, out: "mr_result"});
assert.commandWorked(result);

// Examine the logs to find a list of created collections.
const logLines = checkLog.getGlobalLog(primaryDB);
let createdCollections = [];
logLines.forEach(function(line) {
    if (line.match(/createCollection: (.+) with/)) {
        createdCollections.push(matchResult[1]);
    }
});

createdCollections.forEach(function(createdCollectionName) {
    if (createdCollectionName.startsWith("admin.")) {
        // Although the "admin.system.version" collection is replicated, no "c" entry gets
        // created for it in the oplog, so this test would see it as unreplicated. In general,
        // this test is not concerned with the "admin" database, so we don't examine any "admin"
        // collections.
        return;
    }

    const periodIndex = createdCollectionName.indexOf(".");
    const dbName = createdCollectionName.substring(0, periodIndex);
    const collName = createdCollectionName.substring(periodIndex + 1);

    // Search for a log entry for the creation of this collection.
    const oplogEntries =
        primaryDB.getSiblingDB("local")["oplog.rs"]
            .find({op: "c", ns: dbName + ".$cmd", "o.create": collName, "o.idIndex.name": "_id_"})
            .toArray();
    if (createdCollectionName.startsWith("local.")) {
        // We do not want to see any replication of "local" collections.
        assert.eq(oplogEntries.length,
                  0,
                  "Found unexpected oplog entry for creation of " + createdCollectionName + ": " +
                      tojson(oplogEntries));
    } else {
        assert.eq(oplogEntries.length,
                  1,
                  "Found no oplog entry or too many entries for creation of " +
                      createdCollectionName + ": " + tojson(oplogEntries));
    }
});

replSet.stopSet();
}());
