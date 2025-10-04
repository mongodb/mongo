/**
 * Tests the DataConsistencyChecker#getCollectionDiffUsingSessions() method for comparing the
 * contents between a primary and secondary server.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "diff_using_session_test";
const collName = "mycoll";

const primaryDB = rst.getPrimary().startSession().getDatabase(dbName);
const secondaryDB = rst.getSecondary().startSession().getDatabase(dbName);

// The default WC is majority and rsSyncApplyStop failpoint will prevent satisfying any majority
// writes.
assert.commandWorked(
    rst.getPrimary().adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

assert.commandWorked(
    primaryDB[collName].insert(
        Array.from({length: 100}, (_, i) => ({_id: i, num: i * 2})),
        {writeConcern: {w: 2}},
    ),
);

// There should be no missing or mismatched documents after having waited for replication.
let diff = DataConsistencyChecker.getCollectionDiffUsingSessions(
    primaryDB.getSession(),
    secondaryDB.getSession(),
    dbName,
    collName,
);

assert.eq(diff, {docsWithDifferentContents: [], docsMissingOnSource: [], docsMissingOnSyncing: []});

// We pause replication on the secondary to intentionally cause the contents between the primary and
// the secondary to differ.
assert.commandWorked(secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
checkLog.contains(secondaryDB, "rsSyncApplyStop fail point enabled. Blocking until fail point is disabled");

const expectedMissingOnSecondary = [
    {_id: 30.2, num: -1},
    {_id: 70.4, num: -2},
];
const expectedMissingOnPrimary = [
    {_id: 10, num: 20},
    {_id: 50, num: 100},
];

assert.commandWorked(primaryDB[collName].insert(expectedMissingOnSecondary));
assert.commandWorked(
    primaryDB[collName].remove({_id: {$in: expectedMissingOnPrimary.map((doc) => doc._id)}}, {justOne: false}),
);
assert.commandWorked(primaryDB[collName].update({_id: {$in: [40, 90]}}, {$set: {extra: "yes"}}, {multi: true}));

// Type fidelity is expected to be preserved by replication so intentionally test comparisons of
// distinct but equivalent BSON types.
assert.commandWorked(primaryDB[collName].update({_id: 2}, {$set: {num: NumberLong(4)}}));

diff = DataConsistencyChecker.getCollectionDiffUsingSessions(
    primaryDB.getSession(),
    secondaryDB.getSession(),
    dbName,
    collName,
);

assert.eq(diff, {
    docsWithDifferentContents: [
        {sourceNode: {_id: 2, num: NumberLong(4)}, syncingNode: {_id: 2, num: 4}},
        {sourceNode: {_id: 40, num: 80, extra: "yes"}, syncingNode: {_id: 40, num: 80}},
        {sourceNode: {_id: 90, num: 180, extra: "yes"}, syncingNode: {_id: 90, num: 180}},
    ],
    docsMissingOnSource: expectedMissingOnPrimary,
    docsMissingOnSyncing: expectedMissingOnSecondary,
});

assert.commandWorked(secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));

rst.stopSet();
