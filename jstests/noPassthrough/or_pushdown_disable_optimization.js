/**
 * Test that queries eligible for OR-pushdown optimization do not crash the server when the
 * 'disableMatchExpressionOptimization' failpoint is enabled.
 *
 * Originally designed to reproduce SERVER-70597.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

const coll = db.getCollection(jsTestName());
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

let docs = [];
for (let a = 1; a <= 3; ++a) {
    for (let b = 1; b <= 3; ++b) {
        docs.push({a, b});
    }
}
assert.commandWorked(coll.insert(docs));

// This query has a nested $and, and a one-argument contained $or. Normally we canonicalize this
// predicate by flattening the $and and unwrapping the $or. The OR-pushdown optimization assumes the
// predicate has been canonicalized, but this assumption is broken by the failpoint.
const results = coll.aggregate([
                        {$match: {$and: [{$and: [{a: 2}]}, {$or: [{b: 3}]}]}},
                        {$unset: "_id"},
                    ])
                    .toArray();
assert.eq(results, [{a: 2, b: 3}]);

MongoRunner.stopMongod(conn);
})();
