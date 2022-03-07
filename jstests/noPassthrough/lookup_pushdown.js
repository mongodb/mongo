/**
 * Tests basic functionality of pushing $lookup into the find layer.
 *
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");      // For 'checkSBEEnabled()'.
load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStages()'.

const JoinAlgorithm = {
    Classic: 0,
    NLJ: 1,
    // These two joins aren't implemented yet and will throw errors with the corresponding codes.
    HJ: 5842602,
    INLJ: 5842603,
};

// Standalone cases.
const conn = MongoRunner.runMongod({setParameter: "featureFlagSBELookupPushdown=true"});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
const foreignCollName = "foreign_lookup_pushdown";
const viewName = "view_lookup_pushdown";

function runTest(coll, pipeline, expectedJoinAlgorithm, aggOptions = {}, errMsgRegex = null) {
    const options = Object.assign({pipeline, cursor: {}}, aggOptions);
    const response = coll.runCommand("aggregate", options);

    if (expectedJoinAlgorithm === JoinAlgorithm.Classic) {
        assert.commandWorked(response);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

        // In the classic case, verify that $lookup was not lowered into SBE. Note that we don't
        // check for the presence of $lookup agg stages because in the sharded case, $lookup will
        // not execute on each shard and will not show up in the output of 'getAggPlanStages'.
        assert.eq(eqLookupNodes.length,
                  0,
                  "there should be no lowered EQ_LOOKUP stages; got " + tojson(explain));
    } else if (expectedJoinAlgorithm === JoinAlgorithm.HJ ||
               expectedJoinAlgorithm === JoinAlgorithm.INLJ) {
        const result = assert.commandFailedWithCode(response, expectedJoinAlgorithm);
        if (errMsgRegex) {
            const errorMessage = result.errmsg;
            assert(errMsgRegex.test(errorMessage),
                   "Error message '" + errorMessage + "' did not match the RegEx '" + errMsgRegex +
                       "'");
        }
    } else {
        assert.commandWorked(response);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

        // Verify via explain that $lookup was lowered and NLJ was chosen.
        assert.gt(eqLookupNodes.length,
                  0,
                  "expected at least one EQ_LOOKUP node; got " + tojson(explain));

        // Fetch the deepest EQ_LOOKUP node.
        const eqLookupNode = eqLookupNodes[eqLookupNodes.length - 1];
        assert(eqLookupNode, "expected EQ_LOOKUP node; explain: " + tojson(explain));
        const strategy = eqLookupNode.strategy;
        assert(strategy, "expected EQ_LOOKUP node to have a strategy " + tojson(eqLookupNode));
        assert.eq("NestedLoopJoin",
                  strategy,
                  "Incorrect strategy; expected NestedLoopJoin, got " + tojson(strategy));
    }
}

let db = conn.getDB(name);
if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    jsTestLog("Skipping test because either the sbe lookup pushdown feature flag is disabled or" +
              " sbe itself is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

let coll = db[name];
assert.commandWorked(coll.insert({_id: 1, a: 2}));
let foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({_id: 0, b: 2, c: 2}));
assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
let view = db[viewName];

// Basic $lookup.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// TODO SERVER-64091: Add a test case for pushed down $lookup against a non-existent foreign
// collection.

// Self join $lookup, no views.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Self join $lookup; left hand is a view. This is expected to be pushed down because the view
// pipeline itself is a $match, which is eligible for pushdown.
runTest(view,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Self join $lookup; right hand is a view.
runTest(coll,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Self join $lookup; both namespaces are views.
runTest(view,
        [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup preceded by $match.
runTest(coll,
        [
            {$match: {a: {$gte: 0}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// $lookup preceded by $project.
runTest(coll,
        [
            {$project: {a: 1}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// $lookup preceded by $project which features an SBE-incompatible expression.
// TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
runTest(coll,
        [
            {$project: {exp: {$pow: ["$a", 3]}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup preceded by $group.
runTest(coll,
        [
            {$group: {_id: "$a", sum: {$sum: 1}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// $lookup preceded by $group that is not eligible for pushdown.
// TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
runTest(coll,
        [
            {$group: {_id: {$pow: ["$a", 3]}, sum: {$sum: 1}}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Consecutive $lookups, where the first $lookup is against a view.
runTest(coll,
        [
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Consecutive $lookups, where the first $lookup is against a regular collection. Here, neither
// $lookup is eligible for pushdown because currently, we can only know whether any secondary
// collection is a view or a sharded collection.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup with pipeline.
runTest(coll,
    [{$lookup: {from: foreignCollName, let: {foo: "$b"}, pipeline: [{$match: {$expr: {$eq: ["$$foo",
2]}}}], as: "out"}}], JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup that absorbs $unwind.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup that absorbs $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$unwind: "$out"},
            {$match: {out: {$gte: 0}}}
        ],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// $lookup that does not absorb $match.
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
            {$match: {out: {$gte: 0}}}
        ],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Run a $lookup with 'allowDiskUse' enabled. Because the foreign collection is very small, we
// should select hash join.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.HJ /* expectedJoinAlgorithm */,
        {allowDiskUse: true});

// Build an index on the foreign collection that matches the foreignField. This should cause us
// to choose an indexed nested loop join.
assert.commandWorked(foreignColl.createIndex({b: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_1, \)$//* errMsgRegex */);

// Build a hashed index on the foreign collection that matches the foreignField. Indexed nested loop
// join strategy should be used.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_hashed, \)$//* errMsgRegex */);

// Build a wildcard index on the foreign collection that matches the foreignField. Nested loop join
// strategy should be used.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({'$**': 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Build a compound index that is prefixed with the foreignField. We should use an indexed
// nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_1_c_1_a_1, \)$//* errMsgRegex */);

// Build multiple compound indexes prefixed with the foreignField. We should utilize the index with
// the least amount of components.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_1_a_1, \)$//* errMsgRegex */);

// In the presence of hashed and BTree indexes with the same number of components, we should select
// BTree one.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1}));
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_1, \)$//* errMsgRegex */);

// While selecting a BTree index is more preferable, we should favor hashed index if it has smaller
// number of components.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, d: 1}));
assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_hashed, \)$//* errMsgRegex */);

// If we have two indexes of the same type with the same number of components, index keypattern
// should be used as a tie breaker.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));
assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
        {} /* aggOptions */,
        /\(b_1_a_1, \)$//* errMsgRegex */);

// Build a 2d index on the foreign collection that matches the foreignField. In this case, we should
// use regular nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: '2d'}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Build a compound index containing the foreignField, but not as the first field. In this case,
// we should use regular nested loop join.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({a: 1, b: 1, c: 1}));
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Multiple $lookup stages in a pipeline that should pick different physical joins.
assert.commandWorked(foreignColl.dropIndexes());
assert.commandWorked(foreignColl.createIndex({b: 1}));
runTest(coll,
        [
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}},
            {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
        ],
        JoinAlgorithm.INLJ /* expectedJoinAlgorithm; The stage with foreignField 'c' will be
         built first and use NLJ with no error while the stage with foreignField 'b' will use
          INLJ and throw an error */);
runTest(
    coll,
    [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
    ],
    JoinAlgorithm.INLJ /* expectedJoinAlgorithm for the second stage, because it's built first */);

// "localField" contains a numeric component (unsupported by SBE).
runTest(coll,
        [{$lookup: {from: name, localField: "a.0", foreignField: "a", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// "foreignField" contains a numeric component (unsupported by SBE).
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "a.0", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// "as" field contains a numeric component (numbers in this field are treated as literal field names
// so this is supported by SBE).
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out.0"}}],
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

MongoRunner.stopMongod(conn);

(function testHashJoinQueryKnobs() {
    // Create a new scope and start a new mongod so that the mongod-wide global state changes do not
    // affect subsequent tests if any.
    const conn = MongoRunner.runMongod({setParameter: "featureFlagSBELookupPushdown=true"});
    const db = conn.getDB(name);
    const lcoll = db.query_knobs_local;
    const fcoll = db.query_knobs_foreign;

    assert.commandWorked(lcoll.insert({a: 1}));
    assert.commandWorked(fcoll.insert([{a: 1}, {a: 1}]));

    // The foreign collection is very small and first verifies that the HJ is chosen under the
    // default query knob values.
    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            {allowDiskUse: true});

    // The fcollStats.count means the number of documents in a collection, the fcollStats.size means
    // the collection's data size, and the fcollStats.storageSize means the allocated storage size.
    const fcollStats = assert.commandWorked(fcoll.stats());
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size,
        internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin: fcollStats.storageSize
    }));

    // Verifies that the HJ is still chosen.
    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            {allowDiskUse: true});

    // Setting the 'internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin' to count - 1 results in
    // choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            {allowDiskUse: true});

    // Reverting back 'internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin' to the previous
    // value. Setting the 'internalQueryCollectionMaxDataSizeBytesToChooseHashJoin' to size - 1
    // results in choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            {allowDiskUse: true});

    // Reverting back 'internalQueryCollectionMaxDataSizeBytesToChooseHashJoin' to the previous
    // value. Setting the 'internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin' to
    // storageSize - 1 results in choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size,
        internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin: fcollStats.storageSize - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            {allowDiskUse: true});

    MongoRunner.stopMongod(conn);
}());

// Sharded cases.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {shardOptions: {setParameter: "featureFlagSBELookupPushdown=true"}}
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
        JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

// Sharded main collection, unsharded right side. This is not expected to be eligible for pushdown
// because the $lookup will be preceded by a $mergeCursors stage on the merging shard.
runTest(coll,
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Both collections are sharded.
runTest(coll,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Unsharded main collection, sharded right side.
runTest(foreignColl,
        [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Unsharded main collection, unsharded view right side.
runTest(foreignColl,
        [{$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

// Unsharded main collection, sharded view on the right side.
runTest(foreignColl,
        [{$lookup: {from: shardedViewName, localField: "a", foreignField: "b", as: "out"}}],
        JoinAlgorithm.Classic /* expectedJoinAlgorithm */);
st.stop();
}());
