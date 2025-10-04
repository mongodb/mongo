/**
 * Confirms slow currentOp logging does not conflict with applying an oplog batch.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000, // Don't log slow operations on secondary.
        },
    ],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({_id: "a"}));
rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const failPoint = configureFailPoint(secondaryDB, "hangAfterCollectionInserts", {
    collectionNS: coll.getFullName(),
});

try {
    assert.commandWorked(coll.insert({_id: "b"}));
    failPoint.wait();

    jsTestLog("Running currentOp() with slow operation logging.");
    // Lower slowms to make currentOp() log slow operation while the secondary is procesing the
    // commitIndexBuild oplog entry during oplog application.
    // Use admin db on secondary to avoid lock conflict with inserts in test db.
    const secondaryAdminDB = secondaryDB.getSiblingDB("admin");
    const profileResult = assert.commandWorked(secondaryAdminDB.setProfilingLevel(0, {slowms: -1}));
    jsTestLog("Configured profiling to always log slow ops: " + tojson(profileResult));
    const currentOpResult = assert.commandWorked(secondaryAdminDB.currentOp());
    jsTestLog("currentOp() with slow operation logging: " + tojson(currentOpResult));
    assert.commandWorked(secondaryAdminDB.setProfilingLevel(profileResult.was, {slowms: profileResult.slowms}));
    jsTestLog("Completed currentOp() with slow operation logging.");
} finally {
    failPoint.off();
}

rst.stopSet();
