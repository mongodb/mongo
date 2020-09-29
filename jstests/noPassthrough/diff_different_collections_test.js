/**
 * Tests the DataConsistencyChecker.getDiff() function can be used to compare the contents between
 * different collections.
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "diff_different_collections_test";
const collName1 = "coll_one";
const collName2 = "coll_two";

const primaryDB = rst.getPrimary().getDB(dbName);

const matchingDocs = Array.from({length: 100}, (_, i) => ({_id: i, num: i * 2}));
assert.commandWorked(primaryDB[collName1].insert(matchingDocs));
assert.commandWorked(primaryDB[collName2].insert(matchingDocs));

let diff = DataConsistencyChecker.getDiff(primaryDB[collName1].find().sort({_id: 1}),
                                          primaryDB[collName2].find().sort({_id: 1}));

assert.eq(diff, {docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []});

const expectedMissingOnSecond = [{_id: 30.2, num: -1}, {_id: 70.4, num: -2}];
const expectedMissingOnFirst = [{_id: 10, num: 20}, {_id: 50, num: 100}];

assert.commandWorked(primaryDB[collName1].insert(expectedMissingOnSecond));
assert.commandWorked(primaryDB[collName1].remove(
    {_id: {$in: expectedMissingOnFirst.map(doc => doc._id)}}, {justOne: false}));
assert.commandWorked(
    primaryDB[collName1].update({_id: {$in: [40, 90]}}, {$set: {extra: "yes"}}, {multi: true}));

// Type fidelity is expected to be preserved by replication so intentionally test comparisons of
// distinct but equivalent BSON types.
assert.commandWorked(primaryDB[collName1].update({_id: 2}, {$set: {num: NumberLong(4)}}));

diff = DataConsistencyChecker.getDiff(primaryDB[collName1].find().sort({_id: 1}),
                                      primaryDB[collName2].find().sort({_id: 1}));

assert.eq(diff,
          {
              docsWithDifferentContents: [
                  {first: {_id: 2, num: NumberLong(4)}, second: {_id: 2, num: 4}},
                  {first: {_id: 40, num: 80, extra: "yes"}, second: {_id: 40, num: 80}},
                  {first: {_id: 90, num: 180, extra: "yes"}, second: {_id: 90, num: 180}},
              ],
              docsMissingOnFirst: expectedMissingOnFirst,
              docsMissingOnSecond: expectedMissingOnSecond
          },
          "actual mismatch between collections differed");

// It is also possible to compare the contents of different collections across different servers.
rst.awaitReplication();
const secondaryDB = rst.getSecondary().getDB(dbName);

diff = DataConsistencyChecker.getDiff(primaryDB[collName1].find().sort({_id: 1}),
                                      secondaryDB[collName2].find().sort({_id: 1}));

assert.eq(diff,
          {
              docsWithDifferentContents: [
                  {first: {_id: 2, num: NumberLong(4)}, second: {_id: 2, num: 4}},
                  {first: {_id: 40, num: 80, extra: "yes"}, second: {_id: 40, num: 80}},
                  {first: {_id: 90, num: 180, extra: "yes"}, second: {_id: 90, num: 180}},
              ],
              docsMissingOnFirst: expectedMissingOnFirst,
              docsMissingOnSecond: expectedMissingOnSecond
          },
          "actual mismatch between servers differed");

rst.stopSet();
})();
