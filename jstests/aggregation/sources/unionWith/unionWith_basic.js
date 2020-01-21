// Basic integration test for the $unionWith stage.
// @tags: [
//     # TODO SERVER-45526 Support $unionWith against a mongos.
//     assumes_against_mongod_not_mongos,
//     # $unionWith is new in 4.4.
//     requires_fcv_44,
//  ]
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
testDB.A.drop();
testDB.B.drop();

// Test that using non-existent collections yields no results.
let results = testDB.A.aggregate([{$unionWith: {coll: "B", pipeline: []}}]).toArray();
assert.eq(results, []);

// Test that it does indeed union the two result sets.
assert.commandWorked(testDB.A.insert([{_id: "A_1"}, {_id: "A_2"}]));
assert.commandWorked(testDB.B.insert([{_id: "B_1"}, {_id: "B_2"}]));
results =
    testDB.A.aggregate([{$unionWith: {coll: "B", pipeline: []}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [{_id: "A_1"}, {_id: "A_2"}, {_id: "B_1"}, {_id: "B_2"}]);

// Test that a custom sub-pipeline is applied.
results =
    testDB.A
        .aggregate(
            [{$unionWith: {coll: "B", pipeline: [{$match: {_id: "B_2"}}]}}, {$sort: {_id: 1}}])
        .toArray();
assert.eq(results, [{_id: "A_1"}, {_id: "A_2"}, {_id: "B_2"}]);

// Test that you can nest one $unionWith inside of another.
results = testDB.A.aggregate([{$unionWith: {coll: "B", pipeline: [{$unionWith: "C"}]}}]).toArray();
assert.eq(results, [{_id: "A_1"}, {_id: "A_2"}, {_id: "B_1"}, {_id: "B_2"}]);
}());
