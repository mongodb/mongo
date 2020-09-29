/**
 * Tests the ReplSetTest#getCollectionDiffUsingSessions() method for comparing the contents between
 * a primary and secondary server.
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "diff_using_session_test";
const collName = "mycoll";

const primaryDB = rst.getPrimary().startSession().getDatabase(dbName);
const secondaryDB = rst.getSecondary().startSession().getDatabase(dbName);

assert.commandWorked(primaryDB[collName].insert(
    Array.from({length: 100}, (_, i) => ({_id: i, num: i * 2})), {writeConcern: {w: 2}}));

// There should be no missing or mismatched documents after having waited for replication.
let diff = rst.getCollectionDiffUsingSessions(
    primaryDB.getSession(), secondaryDB.getSession(), dbName, collName);

assert.eq(diff,
          {docsWithDifferentContents: [], docsMissingOnPrimary: [], docsMissingOnSecondary: []});

// We pause replication on the secondary to intentionally cause the contents between the primary and
// the secondary to differ.
assert.commandWorked(
    secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));

const expectedMissingOnSecondary = [{_id: 30.2, num: -1}, {_id: 70.4, num: -2}];
const expectedMissingOnPrimary = [{_id: 10, num: 20}, {_id: 50, num: 100}];

assert.commandWorked(primaryDB[collName].insert(expectedMissingOnSecondary));
assert.commandWorked(primaryDB[collName].remove(
    {_id: {$in: expectedMissingOnPrimary.map(doc => doc._id)}}, {justOne: false}));
assert.commandWorked(
    primaryDB[collName].update({_id: {$in: [40, 90]}}, {$set: {extra: "yes"}}, {multi: true}));

// Type fidelity is expected to be preserved by replication so intentionally test comparisons of
// distinct but equivalent BSON types.
assert.commandWorked(primaryDB[collName].update({_id: 2}, {$set: {num: NumberLong(4)}}));

diff = rst.getCollectionDiffUsingSessions(
    primaryDB.getSession(), secondaryDB.getSession(), dbName, collName);

assert.eq(diff, {
    docsWithDifferentContents: [
        {primary: {_id: 2, num: NumberLong(4)}, secondary: {_id: 2, num: 4}},
        {primary: {_id: 40, num: 80, extra: "yes"}, secondary: {_id: 40, num: 80}},
        {primary: {_id: 90, num: 180, extra: "yes"}, secondary: {_id: 90, num: 180}},
    ],
    docsMissingOnPrimary: expectedMissingOnPrimary,
    docsMissingOnSecondary: expectedMissingOnSecondary
});

assert.commandWorked(
    secondaryDB.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));

rst.stopSet();
})();
