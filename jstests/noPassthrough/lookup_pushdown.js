/**
 * Tests basic functionality of pushing $lookup into the find layer.
 *
 * @tags: [requires_sharding, uses_transactions]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");      // For 'checkSBEEnabled()'.
load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStages' and other explain helpers.

const JoinAlgorithm = {
    Classic: 0,
    NLJ: 1,
    INLJ: 2,
    HJ: 3,
    NonExistentForeignCollection: 4,
};

// Standalone cases.
const conn =
    MongoRunner.runMongod({setParameter: {featureFlagSbeFull: true, allowDiskUseByDefault: true}});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
const foreignCollName = "foreign_lookup_pushdown";
const viewName = "view_lookup_pushdown";

/**
 * Helper function which verifies that at least one $lookup was lowered into SBE within
 * 'explain', and that the EqLookupNode at 'eqLookupNodeIndex' chose the appropriate strategy.
 * In particular, if 'IndexedLoopJoin' was chosen, we verify that the index described by
 * 'indexKeyPattern' was chosen. Otherwise, we verify that 'NestedLoopJoin' was chosen.
 */
function verifyEqLookupNodeStrategy(
    explain, eqLookupNodeIndex, expectedStrategy, indexKeyPattern = {}) {
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
    assert.gt(
        eqLookupNodes.length, 0, "expected at least one EQ_LOOKUP node; got " + tojson(explain));

    // Verify that we're selecting an EQ_LOOKUP node within range.
    assert(eqLookupNodeIndex >= 0 && eqLookupNodeIndex < eqLookupNodes.length,
           "expected eqLookupNodeIndex of '" + eqLookupNodeIndex +
               "' to be within range of available EQ_LOOKUP nodes; got " + tojson(explain));

    // Fetch the requested EQ_LOOKUP node.
    const eqLookupNode = eqLookupNodes[eqLookupNodes.length - 1 - eqLookupNodeIndex];
    assert(eqLookupNode, "expected EQ_LOOKUP node; explain: " + tojson(explain));
    const strategy = eqLookupNode.strategy;
    assert(strategy, "expected EQ_LOOKUP node to have a strategy " + tojson(eqLookupNode));
    assert.eq(
        expectedStrategy,
        strategy,
        "Incorrect strategy; expected " + tojson(expectedStrategy) + ", got " + tojson(strategy));

    if (strategy === "IndexedLoopJoin") {
        assert(indexKeyPattern,
               "expected indexKeyPattern should be set for IndexedLoopJoin algorithm");
        assert.docEq(eqLookupNode.indexKeyPattern,
                     indexKeyPattern,
                     "expected IndexedLoopJoin node to have index " + tojson(indexKeyPattern) +
                         ", got plan " + tojson(eqLookupNode));
    }
}

function getJoinAlgorithmStrategyName(joinAlgorithm) {
    switch (joinAlgorithm) {
        case JoinAlgorithm.NLJ:
            return "NestedLoopJoin";
        case JoinAlgorithm.INLJ:
        case JoinAlgorithm.INLJHashedIndex:
            return "IndexedLoopJoin";
        case JoinAlgorithm.HJ:
            return "HashJoin";
        case JoinAlgorithm.NonExistentForeignCollection:
            return "NonExistentForeignCollection";
        case JoinAlgorithm.Classic:
        default:
            assert(false, "No strategy for JoinAlgorithm: " + joinAlgorithm);
    }
}

function runTest(coll,
                 pipeline,
                 expectedJoinAlgorithm,
                 indexKeyPattern = null,
                 aggOptions = {},
                 errMsgRegex = null,
                 checkMultiPlanning = false,
                 eqLookupNodeIndex = 0) {
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
    } else {
        assert.commandWorked(response);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const expectedStrategy = getJoinAlgorithmStrategyName(expectedJoinAlgorithm);
        verifyEqLookupNodeStrategy(explain, eqLookupNodeIndex, expectedStrategy, indexKeyPattern);

        // Verify that multiplanning took place by verifying that there was at least one
        // rejected plan.
        if (checkMultiPlanning) {
            assert(hasRejectedPlans(explain), explain);
        }
    }
}

let db = conn.getDB(name);
if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping test because either the sbe lookup pushdown feature flag is disabled or" +
              " sbe itself is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

let coll = db[name];
const localDocs = [{_id: 1, a: 2}];
assert.commandWorked(coll.insert(localDocs));
let foreignColl = db[foreignCollName];
const foreignDocs = [{_id: 0, b: 2, c: 2}];
assert.commandWorked(foreignColl.insert(foreignDocs));
assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
let view = db[viewName];

function setLookupPushdownDisabled(value) {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQuerySlotBasedExecutionDisableLookupPushdown: value}));
}

(function testLookupPushdownQueryKnob() {
    const pipeline =
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}];
    setLookupPushdownDisabled(true);
    runTest(coll, pipeline, JoinAlgorithm.Classic /* expectedJoinAlgorithm */);
    setLookupPushdownDisabled(false);
    runTest(coll, pipeline, JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
}());

(function testLookupPushdownBasicCases() {
    // Basic $lookup.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // $lookup against a non-existent foreign collection should pick NLJ.
    runTest(coll,
            [{$lookup: {from: "nonexistent", localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NonExistentForeignCollection /* expectedJoinAlgorithm */);

    // $lookup against a non-existent foreign collection should pick NLJ even when HJ is eligible.
    runTest(coll,
            [{$lookup: {from: "nonexistent", localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NonExistentForeignCollection /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Self join $lookup, no views.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // Self join $lookup; left hand is a view. This is expected to be pushed down because the view
    // pipeline itself is a $match, which is eligible for pushdown.
    runTest(view,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

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
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // $lookup preceded by $project.
    runTest(coll,
            [
                {$project: {a: 1}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

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
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

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
            [{
                $lookup: {
                    from: foreignCollName, let: {foo: "$b"}, pipeline: [{
                        $match: {
                            $expr: {
                                $eq: ["$$foo",
                                    2]
                            }
                        }
                    }], as: "out"
                }
            }], JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

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
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // Run a $lookup with 'allowDiskUse' enabled. Because the foreign collection is very small, we
    // should select hash join.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {allowDiskUse: true});
}());

// Verify that SBE is only used when a $lookup or a $group is present.
(function testLookupGroupIsRequiredForPushdown() {
    // Don't execute this test case if SBE is fully enabled.
    if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
        jsTestLog("Skipping test case because we are supporting SBE beyond $group and $lookup" +
                  " pushdown");
        return;
    }

    const assertEngineUsed = function(pipeline, isSBE) {
        const explain = coll.explain().aggregate(pipeline);
        assert(explain.hasOwnProperty("explainVersion"), explain);
        if (isSBE) {
            assert.eq(explain.explainVersion, "2", explain);
        } else {
            assert.eq(explain.explainVersion, "1", explain);
        }
    };

    const lookup = {$lookup: {from: "coll", localField: "a", foreignField: "b", as: "out"}};
    const group = {
        $group: {
            _id: "$a",
            out: {$min: "$b"},
        }
    };
    const match = {$match: {a: 1}};

    // $lookup and $group should each run in SBE.
    assertEngineUsed([lookup], true /* isSBE */);
    assertEngineUsed([group], true /* isSBE */);
    assertEngineUsed([lookup, group], true /* isSBE */);

    // $match on its own won't use SBE, nor will an empty pipeline.
    assertEngineUsed([match], false /* isSBE */);
    assertEngineUsed([], false /* isSBE */);

    // $match will use SBE if followed by either a $group or a $lookup.
    assertEngineUsed([match, lookup], true /* isSBE */);
    assertEngineUsed([match, group], true /* isSBE */);
})();

// Build an index on the foreign collection that matches the foreignField. This should cause us
// to choose an indexed nested loop join.
(function testIndexNestedLoopJoinRegularIndex() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Construct an index with a partial filter expression. In this case, we should NOT use INLJ.
(function testPartialFilterExpressionIndexesAreIgnored() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}, {partialFilterExpression: {b: 1}}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // If we add an index that is not a partial index, we should then use INLJ.
    assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, a: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a hashed index on the foreign collection that matches the foreignField. Indexed nested loop
// join strategy should be used.
(function testIndexNestedLoopJoinHashedIndex() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: "hashed"}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: "hashed"} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a wildcard index on the foreign collection that matches the foreignField. Nested loop join
// strategy should be used.
(function testWildcardIndexInhibitsIndexNestedLoopJoin() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({'$**': 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // Insert a document with multikey paths in the foreign collection that will be used for testing
    // wildcard indexes.
    const mkDoc = {b: [3, 4], c: [5, 6, {d: [7, 8]}]};
    assert.commandWorked(foreignColl.insert(mkDoc));

    // An incompatible wildcard index should result in using NLJ.
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({'b.$**': 1}));
    runTest(coll,
            [{
                $lookup:
                    {from: foreignCollName, localField: "a", foreignField: "not a match", as: "out"}
            }],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // A compatible wildcard index with no other SBE compatible indexes should result in NLJ.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b.c", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({'$**': 1}, {wildcardProjection: {b: 1}}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // Create a regular index over the foreignField. We should now use INLJ.
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());

    // Verify that a leading $match won't filter out a legitimate wildcard index.
    assert.commandWorked(foreignColl.createIndex({'$**': 1}, {wildcardProjection: {b: 1, c: 1}}));
    runTest(coll,
            [
                {$match: {'c.d': 1}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.deleteOne(mkDoc));
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a compound index that is prefixed with the foreignField. We should use an indexed
// nested loop join.
(function testCompoundIndexWithForeignFieldPrefix() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, c: 1, a: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build multiple compound indexes prefixed with the foreignField. We should utilize the index with
// the least amount of components.
(function testIndexWithFewestComponentsIsUsed() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, a: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

(function testBTreeIndexChosenOverHashedIndex() {
    // In the presence of hashed and BTree indexes with the same number of components, we should
    // select BTree one.
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// While selecting a BTree index is more preferable, we should favor hashed index if it has
// smaller number of components.
(function testFewerComponentsFavoredOverIndexType() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, d: 1}));
    assert.commandWorked(foreignColl.createIndex({b: "hashed"}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: "hashed"} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// If we have two indexes of the same type with the same number of components, index keypattern
// should be used as a tie breaker.
(function testIndexKeyPatternUsedAsTieBreaker() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, a: 1});
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Build a 2d index on the foreign collection that matches the foreignField. In this case, we should
// use regular nested loop join.
(function testNonBTreeOrHashedIndexesNotUsedForPushdown() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: '2d'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Build a sparse index on the foreign collection that matches the foreignField. In this case, we
// should use regular nested loop join.
(function testSparseIndexesNotUsedForPushDown() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}, {sparse: true}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Build a compound index containing the foreignField, but not as the first field. In this case,
// we should use regular nested loop join.
(function testForeignFieldNotPrefixInhibitsIndexNestedLoopJoin() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({a: 1, b: 1, c: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Multiple $lookup stages in a pipeline that should pick different physical joins.
(function testMultipleLookupStagesPickDifferentPhysicalJoins() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));

    let pipeline = [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
    ];
    runTest(coll, pipeline, JoinAlgorithm.INLJ /* expectedJoinAlgorithm */, {
        b: 1
    } /* indexKeyPattern */);
    runTest(coll,
            pipeline,
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {} /* aggOptions */,
            null /* errMsgRegex */,
            false /* checkMultiPlanning */,
            1 /* eqLookupNodeIndex */);

    pipeline = [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
    ];
    runTest(coll, pipeline, JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
    runTest(coll,
            pipeline,
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */,
            {} /* aggOptions */,
            null /* errMsgRegex */,
            false /* checkMultiPlanning */,
            1 /* eqLookupNodeIndex */);

    assert.commandWorked(foreignColl.dropIndexes());
})();

(function testNumericComponentsBehaviorForPushdown() {
    // "localField" contains a numeric component (unsupported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a.0", foreignField: "a", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // "foreignField" contains a numeric component (unsupported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a.0", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // "as" field contains a numeric component (numbers in this field are treated as literal field
    // names so this is supported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out.0"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
}());

(function testLocalOrForeignFieldsWithPaths() {
    // "localField" is a path.
    runTest(coll,
            [{$lookup: {from: name, localField: "a.b", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // "foreignField" is a path.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a.b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // "as" field is a path.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out.b"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);
}());

// Verify that $lookup pushdown works correctly in the presence of multi-planning.
(
    function testLookupPushdownWorksWithMultiplanning() {
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.createIndexes([{a: 1, b: 1}, {a: 1, c: 1}]));

        // Verify that $lookup still gets pushed down when the pipeline prefix is pushed down and
        // undergoes multi-planning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
            ],
            JoinAlgorithm.HJ, /* expectedJoinAlgorithm */
            null,             /* indexKeyPattern */
            {},               /* aggOptions */
            null,             /* errMsgRegex */
            true /* checkMultiplanning */);

        // Verify that multiple $lookups will still get pushed down when the pipeline prefix is
        // pushed down and undergoes multi-planning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
            ],
            JoinAlgorithm.HJ, /* expectedJoinAlgorithm */
            null,             /* indexKeyPattern */
            {},               /* aggOptions */
            null,             /* errMsgRegex */
            true /* checkMultiplanning */);

        // Verify that $lookup and $group both get pushed down in the presence of multiplanning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$group: {_id: "$a", groupOut: {$sum: 1}}},
                {
                    $lookup: {
                        from: foreignCollName,
                        localField: "groupOut",
                        foreignField: "c",
                        as: "c_out"
                    }
                }
            ],
            JoinAlgorithm.HJ, /* expectedJoinAlgorithm */
            null, /* indexKeyPattern */
            {},                /* aggOptions */
            null,              /* errMsgRegex */
            true /* checkMultiplanning */);

        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
                {$group: {_id: "$c_out", groupOut: {$sum: 1}}},
            ],
            JoinAlgorithm.HJ, /* expectedJoinAlgorithm */
            null,             /* indexKeyPattern */
            {},               /* aggOptions */
            null,             /* errMsgRegex */
            true /* checkMultiplanning */);
        assert.commandWorked(coll.dropIndexes());
    })();

// Verify that $lookup is correctly pushed down when it is nested inside of a $unionWith.
(
    function
        verifyLookupNestedInUnionWithGetsPushedDown() {
            const unionCollName = "unionColl";
            const unionColl = db[unionCollName];
            assert.commandWorked(unionColl.insert({}));
            const explain = coll.explain().aggregate([{$unionWith: {coll: unionCollName, pipeline: [{$lookup: {from:
                foreignCollName, localField: "a", foreignField: "b", as: "results"}}]}}]);
            const unionWithStage = getAggPlanStage(explain, "$unionWith");
            const unionWithSpec = unionWithStage["$unionWith"];
            assert(unionWithSpec.hasOwnProperty("pipeline"), unionWithSpec);

            // Wrap the subpipeline's explain output in a format that can be parsed by
            // 'getAggPlanStages'.
            verifyEqLookupNodeStrategy({stages: unionWithSpec["pipeline"]},
                                       0,
                                       getJoinAlgorithmStrategyName(JoinAlgorithm.HJ));
            assert(unionColl.drop());
        }());

// Test which verifies that the right side of a classic $lookup is never lowered into SBE, even if
// the queries for the right side are eligible on their own to run in SBE.
(function verifyThatClassicLookupRightSideIsNeverLoweredIntoSBE() {
    // If running with SBE fully enabled, verify that our $match is SBE compatible. Otherwise,
    // verify that the same $match, when used as a $lookup sub-pipeline, will not be lowered
    // into SBE.
    const subPipeline = [{$match: {b: 2}}];
    if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
        const subPipelineExplain = foreignColl.explain().aggregate(subPipeline);
        assert(subPipelineExplain.hasOwnProperty("explainVersion"), subPipelineExplain);
        assert.eq(subPipelineExplain["explainVersion"], "2", subPipelineExplain);
    } else {
        const pipeline = [{$lookup: {from: foreignCollName, pipeline: subPipeline, as: "result"}}];
        runTest(coll, pipeline, JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

        // Run the pipeline enough times to generate a cache entry for the right side in the foreign
        // collection.
        coll.aggregate(pipeline).itcount();
        coll.aggregate(pipeline).itcount();

        const cacheEntries = foreignColl.getPlanCache().list();
        assert.eq(cacheEntries.length, 1);
        const cacheEntry = cacheEntries[0];

        // The cached plan should be a classic plan.
        assert(cacheEntry.hasOwnProperty("version"), cacheEntry);
        assert.eq(cacheEntry.version, "1", cacheEntry);
        assert(cacheEntry.hasOwnProperty("cachedPlan"), cacheEntry);
        const cachedPlan = cacheEntry.cachedPlan;

        // The cached plan should not have slot based plan. Instead, it should be a FETCH + IXSCAN
        // executed in the classic engine.
        assert(!cachedPlan.hasOwnProperty("slots"), cacheEntry);
        assert(cachedPlan.hasOwnProperty("stage"), cacheEntry);

        assert(planHasStage(db, cachedPlan, "FETCH"), cacheEntry);
        assert(planHasStage(db, cachedPlan, "IXSCAN"), cacheEntry);
    }
}());

MongoRunner.stopMongod(conn);

// Verify that pipeline stages get pushed down according to the subset of SBE that is enabled.
(function verifyPushdownLogicSbePartiallyEnabled() {
    const conn = MongoRunner.runMongod({setParameter: {allowDiskUseByDefault: true}});
    const db = conn.getDB(name);
    if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
        jsTestLog("Skipping test case because SBE is fully enabled, but this test case assumes" +
                  " that it is not fully enabled");
        MongoRunner.stopMongod(conn);
        return;
    }
    const coll = db[name];
    const foreignColl = db[foreignCollName];

    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(foreignColl.insert([{b: 1}, {b: 1}]));

    const lookupStage = {
        $lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "result"}
    };
    const groupStage = {$group: {_id: "$a", avg: {$avg: "$b"}}};
    let pipeline = [lookupStage];
    let explain = coll.explain().aggregate(pipeline);

    // We should have exactly one EQ_LOOKUP nodes and no $lookup stage.
    assert.eq(1, getAggPlanStages(explain, "EQ_LOOKUP").length, explain);
    assert.eq(0, getAggPlanStages(explain, "$lookup").length, explain);

    // Run a pipeline where the $group is eligible for push down.
    pipeline = [groupStage, lookupStage];
    explain = coll.explain().aggregate(pipeline);

    // We should have exactly one EQ_LOOKUP nodes and no $lookup stage.
    assert.eq(1, getAggPlanStages(explain, "EQ_LOOKUP").length, explain);
    assert.eq(0, getAggPlanStages(explain, "$lookup").length, explain);

    // We should have exactly one GROUP node and no $group stages.
    assert.eq(1, getAggPlanStages(explain, "GROUP").length, explain);
    assert.eq(0, getAggPlanStages(explain, "$group").length, explain);

    // Run a pipeline where only the first $group is eligible for push down, but the rest of the
    // stages are not.
    pipeline = [groupStage, lookupStage, groupStage, lookupStage];
    explain = coll.explain().aggregate(pipeline);

    // We should have two EQ_LOOKUP nodes and no $lookup stage.
    assert.eq(2, getAggPlanStages(explain, "EQ_LOOKUP").length, explain);
    assert.eq(0, getAggPlanStages(explain, "$lookup").length, explain);

    // We should have two GROUP nodes and no $group stage.
    assert.eq(2, getAggPlanStages(explain, "GROUP").length, explain);
    assert.eq(0, getAggPlanStages(explain, "$group").length, explain);

    function assertEngine(pipeline, engine) {
        const explain = coll.explain().aggregate(pipeline);
        assert(explain.hasOwnProperty("explainVersion"), explain);
        assert.eq(explain.explainVersion, engine === "sbe" ? "2" : "1");
    }

    const matchStage = {$match: {a: 1}};

    // $group on its own is SBE compatible.
    assertEngine([groupStage], "sbe" /* engine */);

    // $group with $match is also SBE compatible.
    assertEngine([matchStage, groupStage], "sbe" /* engine */);

    // A HJ-processed $lookup is also SBE compatible.
    assertEngine([lookupStage], "sbe" /* engine */);
    assertEngine([matchStage, lookupStage], "sbe" /* engine */);
    assertEngine([matchStage, lookupStage, groupStage], "sbe" /* engine */);

    // Constructing an index over the foreignField of 'lookupStage' will cause the $lookup to be
    // pushed down.
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    assertEngine([matchStage, lookupStage, groupStage], "sbe" /* engine */);
    assert.commandWorked(foreignColl.dropIndex({b: 1}));

    // Regardless of whether the $lookup will not run in SBE, a preceding $group should still let
    // SBE be used.
    assertEngine([matchStage, groupStage, lookupStage], "sbe" /* engine */);
    MongoRunner.stopMongod(conn);
}());

(function testHashJoinQueryKnobs() {
    // Create a new scope and start a new mongod so that the mongod-wide global state changes do not
    // affect subsequent tests if any.
    const conn = MongoRunner.runMongod({setParameter: {featureFlagSbeFull: true}});
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
            null /* indexKeyPattern */,
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
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Setting the 'internalQueryDisableLookupExecutionUsingHashJoin' knob to true will disable
    // HJ plans from being chosen and since the pipeline is SBE compatible it will fallback to
    // NLJ.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryDisableLookupExecutionUsingHashJoin: true,
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Test that we can go back to generating HJ plans.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryDisableLookupExecutionUsingHashJoin: false,
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            null /* indexKeyPattern */,
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
            null /* indexKeyPattern */,
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
            null /* indexKeyPattern */,
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
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    MongoRunner.stopMongod(conn);
}());

// Verify that $lookup works in transaction.
(function verifyLookupInTransaction() {
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    function runTransactionTest(pipeline, aggOptions = {}) {
        // Clear the collections.
        primary.getDB(name).getCollection(name).drop();
        primary.getDB(name).getCollection(foreignCollName).drop();

        // Start a snapshot transaction.
        const session = primary.startSession({causalConsistency: false});
        const db = session.getDatabase(name);
        const coll = db.getCollection(name);
        const foreignColl = db.getCollection(foreignCollName);
        assert.commandWorked(coll.insert({_id: 0, a: 0}));
        assert.commandWorked(foreignColl.insert({_id: 0, b: 0}));
        session.startTransaction({readConcern: {level: "snapshot"}});

        function verifySingleDoc(cursor) {
            assert.docEq(cursor.next(), {_id: 0, a: 0, out: [{_id: 0, b: 0}]});
            assert(!cursor.hasNext());
        }

        // Transaction starts with single doc.
        let cursor = coll.aggregate(
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}]);
        verifySingleDoc(cursor);

        // Insert a document outside the transaction, should not be visible in the transaction.
        assert.commandWorked(primary.getDB(name).getCollection(name).insert({_id: "outside_txn"}));

        cursor = coll.aggregate(pipeline, aggOptions);
        verifySingleDoc(cursor);

        assert.commandWorked(session.commitTransaction_forTesting());
    }

    // Basic $lookup should exercise NLJ.
    runTransactionTest(
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
        {allowDiskUse: false});

    // $lookup with index on '_id' foreign field should exercise INLJ.
    runTransactionTest(
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "_id", as: "out"}}],
        {allowDiskUse: false});

    // $lookup with 'allowDiskUse' should exercise HJ.
    runTransactionTest(
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}]);

    assert.commandWorked(primary.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1,
    }));

    // $lookup with HJ in transaction still works with spilling.
    runTransactionTest(
        [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}]);

    rst.stopSet();
}());

// Sharded cases.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {shardOptions: {setParameter: {featureFlagSbeFull: true, allowDiskUseByDefault: true}}}
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

(function testLookupPushdownAgainstShardedCluster() {
    // Both collections are unsharded.
    runTest(foreignColl,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */);

    // Sharded main collection, unsharded right side. This is not expected to be eligible for
    // pushdown because the $lookup will be preceded by a $mergeCursors stage on the merging shard.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Sharded main collection, unsharded right side. Here, we are targeting a single shard, so
    // there will be no leading $mergeCursors stage. We should still avoid pushing down $lookup.
    const singleShardPipeline = [
        {$match: {shardKey: 1}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
    ];
    runTest(coll, singleShardPipeline, JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Verify that the above pipeline targets a single shard and doesn't use a $mergeCursors stage.
    const singleShardExplain = coll.explain().aggregate(singleShardPipeline);
    assert(!aggPlanHasStage(singleShardExplain,
                            "$mergeCursors",
                            "found $mergeCursors in " + tojson(singleShardExplain)));
    assert(singleShardExplain.hasOwnProperty("shards"),
           "should have shards property in explain: " + tojson(singleShardExplain));
    assert.eq(Object.keys(singleShardExplain["shards"]).length,
              1,
              "sharded explain should only" +
                  " target one shard " + tojson(singleShardExplain));

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
}());
st.stop();
}());
