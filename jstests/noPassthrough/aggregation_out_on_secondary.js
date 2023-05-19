/**
 * Basic test to verify that an aggregation pipeline using $out targeting a replica set secondary
 * performs the reads on the secondaries and executes any write commands against the replica set
 * primary.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // for anyEq.

const nDocs = 100;
const readCollName = "readColl";
const outCollName = "outColl";
const dbName = "out_on_secondary_db";
let rs = new ReplSetTest({nodes: 2});
rs.startSet();
rs.initiate();
rs.awaitReplication();

const primary = rs.getPrimary();
const primaryDB = primary.getDB(dbName);
const readColl = primaryDB[readCollName];

const secondary = rs.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const replSetConn = new Mongo(rs.getURL());
replSetConn.setReadPref("secondary");
const db = replSetConn.getDB(dbName);
// Keeps track of values which we expect the aggregate command to return.
let expectedResults = [];
// Insert some documents which our pipeline will eventually read from.
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(
        readColl.insert({_id: i, groupKey: i % 10, num: i}, {writeConcern: {w: 2}}));
    if (i < 10) {
        expectedResults.push({_id: i, sum: i});
    } else {
        expectedResults[i % 10].sum += i;
    }
}

assert.commandWorked(primaryDB.setProfilingLevel(2));
assert.commandWorked(secondaryDB.setProfilingLevel(2));

const pipeline = [{$group: {_id: "$groupKey", sum: {$sum: "$num"}}}, {$out: outCollName}];
const comment = "$out issued to secondary";
assert.eq(db[readCollName].aggregate(pipeline, {comment: comment}).itcount(), 0);

// Verify that $out wrote to the primary and that query is correct.
assert(anyEq(primaryDB[outCollName].find().toArray(), expectedResults));

// Verify that $group was executed on the secondary.
const secondaryProfile =
    secondaryDB.system.profile.find({"command.aggregate": "readColl", "command.comment": comment})
        .itcount();
assert.eq(1, secondaryProfile);

// Verify $out's operations were executed on the primary.
const primaryProfile =
    primaryDB.system.profile
        .find({"command.internalRenameIfOptionsAndIndexesMatch": 1, "command.comment": comment})
        .itcount();
assert.eq(1, primaryProfile);

rs.stopSet();
}());
