/**
 * Tests basic functionality of pushing $lookup into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const JoinAlgorithm = {
    HJ: 5842602,
    INLJ: 5842603,
    NLJ: 5842604,
};

// Standalone cases.
const conn = MongoRunner.runMongod({setParameter: "internalEnableMultipleAutoGetCollections=true"});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
let db = conn.getDB(name);

function runTest(coll, pipeline, expectedCode, aggOptions = {}, errMsgRegex = null) {
    const options = Object.assign({pipeline, cursor: {}}, aggOptions);
    const response = coll.runCommand("aggregate", options);
    if (expectedCode) {
        const result = assert.commandFailedWithCode(response, expectedCode);
        if (errMsgRegex) {
            const errorMessage = result.errmsg;
            assert(errMsgRegex.test(errorMessage),
                   "Error message '" + errorMessage + "' did not match the RegEx '" + errMsgRegex +
                       "'");
        }
    } else {
        assert.commandWorked(response);
    }
}

if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    jsTestLog("Skipping test because the sbe lookup pushdown feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const foreignCollName = "foreign_lookup_pushdown";
const viewName = "view_lookup_pushdown";
let coll = db[name];
assert.commandWorked(coll.insert({_id: 1, a: 2}));
let foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({_id: 0, b: 2, c: 2}));
assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
let view = db[viewName];

// Basic $lookup.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Self join $lookup, no views.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Self join $lookup; left hand is a view. This is expected to be pushed down because the view
// pipeline itself is a $match, which is eligible for pushdown.
runTest(view,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Self join $lookup; right hand is a view.
runTest(coll,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        false /* expectedCode */);

// Self join $lookup; both namespaces are views.
runTest(view,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        false /* expectedCode */);

// $lookup preceded by $match.
runTest(coll,
        [
            {$match: {a: {$gte: 0}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedCode */);

// $lookup preceded by $project.
runTest(coll,
        [
            {$project: {a: 1}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedCode */);

// $lookup preceded by $project which features an SBE-incompatible expression.
// TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
runTest(coll,
        [
            {$project: {exp: {$pow: ["$a", 3]}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        false /* expectedCode */);

// $lookup preceded by $group.
runTest(coll,
        [
            {$group: {_id: "$a", sum: {$sum: 1}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedCode */);

// Consecutive $lookups (first is against view).
runTest(coll,
        [
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        false /* expectedCode */);

// Consecutive $lookups (first is against regular collection).
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedCode */);

// $lookup with pipeline.
runTest(coll,
    [{$lookup: {from: foreignCollName, let: {foo: "$b"}, pipeline: [{$match: {$expr: {$eq: ["$$foo",
2]}}}], as: "out"}}], false /* expectedCode */);

// $lookup that absorbs $unwind.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"}
        ],
        false /* expectedCode */);

// $lookup that absorbs $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"},
            {$match: {out: {$gte: 0}}}
        ],
        false /* expectedCode */);

// $lookup that does not absorb $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$match: {out: {$gte: 0}}}
        ],
        JoinAlgorithm.NLJ /* expectedCode */);

// Run a $lookup with 'allowDiskUse' enabled. Because the foreign collection is very small, we
// should select hash join.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.HJ /* expectedCode */,
        {allowDiskUse: true});

// Build an index on the foreign collection that matches the foreignField. This should cause us
// to choose an indexed nested loop join.
assert.commandWorked(foreignColl.createIndex({b: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_1, \)$//* errMsgRegex */);

// Build a hashed index on the foreign collection that matches the foreignField. Indexed nested loop
// join strategy should be used.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_hashed, \)$//* errMsgRegex */);

// Build a wildcard index on the foreign collection that matches the foreignField. Nested loop join
// strategy should be used.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({'$**': 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Build a compound index that is prefixed with the foreignField. We should use an indexed
// nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_1_c_1_a_1, \)$//* errMsgRegex */);

// Build multiple compound indexes prefixed with the foreignField. We should utilize the index with
// the least amount of components.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_1_a_1, \)$//* errMsgRegex */);

// In the presence of hashed and BTree indexes with the same number of components, we should select
// BTree one.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1}));
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_1, \)$//* errMsgRegex */);

// While selecting a BTree index is more preferable, we should favor hashed index if it has smaller
// number of components.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, d: 1}));
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_hashed, \)$//* errMsgRegex */);

// If we have two indexes of the same type with the same number of components, index keypattern
// should be used as a tie breaker.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));
assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedCode */,
        {} /* aggOptions */,
        /\(b_1_a_1, \)$//* errMsgRegex */);

// Build a 2d index on the foreign collection that matches the foreignField. In this case, we should
// use regular nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: '2d'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Build a compound index containing the foreignField, but not as the first field. In this case,
// we should use regular nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({a: 1, b: 1, c: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Multiple $lookup stages in a pipeline that should pick different physical joins.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1}));
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
        ],
        JoinAlgorithm.NLJ /* expectedCode for the second stage, because it's built first */);
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
        ],
        JoinAlgorithm.INLJ /* expectedCode for the second stage, because it's built first */);

MongoRunner.stopMongod(conn);

// Sharded cases.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {shardOptions: {setParameter: "internalEnableMultipleAutoGetCollections=true"}}
});
db = st.s.getDB(name);

// Setup. Here, 'coll' is sharded, 'foreignColl' is unsharded, 'viewName' is an unsharded view,
// and 'shardedViewName' is a sharded view.
const shardedViewName = "sharded_foreign_view";
coll = db[name];
assert.commandWorked(coll.insert({a: 1, shardKey: 1}));
assert.commandWorked(coll.insert({a: 2, shardKey: 10}));
assert.commandWorked(coll.createIndex({shardKey: 1}));
st.shardColl(coll.getName(), {shardKey: 1}, {shardKey: 5}, {shardKey: 5}, name);

foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({b: 5}));

assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
assert.commandWorked(db.createView(shardedViewName, name, [{$match: {b: {$gte: 0}}}]));

// Both collections are unsharded.
runTest(foreignColl,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedCode */);

// Sharded main collection, unsharded right side. This is not expected to be eligible for pushdown
// because the $lookup will be preceded by a $mergeCursors stage on the merging shard.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedCode */);

// Both collections are sharded.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedCode */);

// Unsharded main collection, sharded right side.
runTest(foreignColl,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedCode */);

// Unsharded main collection, unsharded view right side.
runTest(foreignColl,
        [{$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedCode */);

// Unsharded main collection, sharded view on the right side.
runTest(foreignColl,
        [{$lookup: {from: shardedViewName, localField: "a", foreignField: "b", as: "out"}}],
        false /* expectedCode */);
st.stop();
}());
