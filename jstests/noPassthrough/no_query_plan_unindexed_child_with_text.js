/**
 * Tests that query planning fails when an $or has a text child along with an unindexed child.
 *
 * @tags: [
 *  requires_fcv_70,
 * ]
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");
const coll = db.getCollection(jsTestName());
coll.drop();

assert.commandWorked(coll.insert({x: 1}));

assert.commandWorked(coll.createIndex({"$**": "text"}));

assert.commandWorked(coll.createIndex({"indexed": 1}));

const pipeline = [
    {
        $match: {
            $and: [{
                $and: [
                    {"indexed": {$eq: 1}},
                    {
                        $or: [
                            {$text: {$search: "abcd"}},
                            {"unindexed": {$eq: 1}},
                        ]
                    },
                ]
            }]
        }
    },
];

assert.throwsWithCode(function() {
    coll.aggregate(pipeline);
}, ErrorCodes.NoQueryExecutionPlans);

assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

assert.throwsWithCode(function() {
    coll.aggregate(pipeline);
}, ErrorCodes.NoQueryExecutionPlans);

MongoRunner.stopMongod(conn);
})();
