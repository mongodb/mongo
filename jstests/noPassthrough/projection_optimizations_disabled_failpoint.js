/**
 * This test ensures that projections works correctly in multiplanner with disabled
 * 'disablePipelineOptimization' failpoint.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load('jstests/libs/analyze_plan.js');

const conn = MongoRunner.runMongod();
const db = conn.getDB("TestDatabase");
const coll = db.projection;
coll.drop();

coll.drop();
coll.createIndexes([{a: 1}, {a: -1}]);
coll.insertMany([{a: 1}, {a: 2}]);

db.adminCommand({"configureFailPoint": 'disablePipelineOptimization', "mode": 'alwaysOn'});

const pipeline = [
    {$match: {"a": 1}},
    {$project: {"b": {$and: ["hi", {$toString: {$regexFind: {input: "$c", regex: "yeah"}}}]}}}
];

const result = coll.aggregate(pipeline).toArray();
assert.eq(1, result.length);
MongoRunner.stopMongod(conn);
})();
