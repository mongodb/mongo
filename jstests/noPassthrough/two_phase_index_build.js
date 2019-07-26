/**
 * Tests basic functionality of two-phase index builds.
 *
 * @tags: [requires_replication]
 */

(function() {

// For 'assertIndexes'.
load("jstests/noPassthrough/libs/index_build.js");

const replSet = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});

// Allow the createIndexes command to use the index builds coordinator in single-phase mode.
replSet.startSet({setParameter: {enableIndexBuildsCoordinatorForCreateIndexesCommand: true}});
replSet.initiate();

const testDB = replSet.getPrimary().getDB('test');
const coll = testDB.twoPhaseIndexBuild;
const collName = coll.getName();
const secondaryColl = replSet.getSecondary().getDB('test')[collName];

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 1000;
for (let i = 0; i < numDocs; i++) {
    bulk.insert({a: i, b: i});
}
assert.commandWorked(bulk.execute());

// Use index builds coordinator for a two-phase build
assert.commandWorked(testDB.runCommand(
    {twoPhaseCreateIndexes: coll.getName(), indexes: [{key: {a: 1}, name: 'a_1'}]}));

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);
assert.eq(numDocs, coll.find({a: {$gte: 0}}).hint({a: 1}).itcount());

const cmdNs = testDB.getName() + ".$cmd";
const localDB = testDB.getSiblingDB("local");
const oplogColl = localDB.oplog.rs;

// Ensure both oplog entries were written to the oplog.
assert.eq(1, oplogColl.find({op: "c", ns: cmdNs, "o.startIndexBuild": collName}).itcount());
assert.eq(1, oplogColl.find({op: "c", ns: cmdNs, "o.commitIndexBuild": collName}).itcount());

// Ensure the secondary builds the index.
replSet.waitForAllIndexBuildsToFinish(testDB.getName(), collName);
IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_", "a_1"]);

// Use index build coordinator for a single-phase index build through the createIndexes
// command.
assert.commandWorked(
    testDB.runCommand({createIndexes: coll.getName(), indexes: [{key: {b: 1}, name: 'b_1'}]}));

IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
assert.eq(numDocs, coll.find({a: {$gte: 0}}).hint({b: 1}).itcount());

// Ensure only one oplog entry was written to the oplog.
assert.eq(1, oplogColl.find({op: "c", ns: cmdNs, "o.createIndexes": collName}).itcount());

// Ensure the secondary builds the index.
replSet.waitForAllIndexBuildsToFinish(testDB.getName(), collName);
IndexBuildTest.assertIndexes(secondaryColl, 3, ["_id_", "a_1", "b_1"]);

replSet.stopSet();
})();
