/**
 * The test asserts that SBE $lookup can correctly make use of partial indices on the foreign collection.
 *
 * @tags: [
 *      requires_sbe,
 *      requires_fcv_90
 *  ]
 */

import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

// Disable HashJoin in this test so that the DILJ lookup strategy is not short-circuited.
const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryFrameworkControl: "trySbeRestricted",
        internalQueryDisableLookupExecutionUsingHashJoin: true,
    },
});
const db = conn.getDB(jsTestName());

const localColl = db.localColl;
const foreignColl = db.foreignColl;

// Runs the $lookup and returns results in a deterministic, comparable shape: matched foreign _ids
// are reduced to a sorted array, and the outer documents are sorted by _id.
function runLookup(pipeline, coll = localColl) {
    return coll
        .aggregate(pipeline)
        .toArray()
        .map((doc) => ({
            _id: doc._id,
            matched: doc.matched.map((m) => m._id).sort((x, y) => x - y),
        }))
        .sort((x, y) => x._id - y._id);
}

function getExplainNode(pipeline, stage, coll = localColl) {
    const explain = coll.explain().aggregate(pipeline);
    return getAggPlanStage(explain, stage);
}

const pipeline = [
    {
        $lookup: {
            from: foreignColl.getName(),
            localField: "a",
            foreignField: "b",
            as: "matched",
        },
    },
];

assert.commandWorked(
    localColl.insertMany([
        {_id: 0, a: 3}, // 3  does NOT satisfy {b: {$gt: 5}} -> collection-scan branch
        {_id: 1, a: 10}, // 10 satisfies                     -> index branch
        {_id: 2, a: 7}, // 7  satisfies                      -> index branch
        {_id: 3, a: 1}, // 1  does NOT satisfy               -> collection-scan branch
        {_id: 4, a: [3, 10]}, // array: 3 fails the filter, so the whole record must use the fallback
        {_id: 5, a: 0}, // null: no foreign match at all
        // Array where EVERY key satisfies {b: {$gt: 5}}. The index branch would still be sound for this
        // doc, but the single-key (size==1) guard sends every multi-key document to the collection-scan
        // fallback regardless. Results must still be complete. This distinguishes "fallback because a
        // key fails the filter" (_id:4) from "fallback because the doc is multi-key" (_id:6).
        {_id: 6, a: [7, 10]},
        // Single key matching a foreign value that is itself an array; exercises the multikey index
        // branch + dedup (the foreign doc must appear exactly once even though the index is multikey).
        {_id: 7, a: 8},
    ]),
);

assert.commandWorked(
    foreignColl.insertMany([
        {_id: 100, b: 3},
        {_id: 101, b: 10},
        {_id: 102, b: 7},
        {_id: 103, b: 1},
        {_id: 104, b: 10},
        {_id: 105, b: 99},
        // Foreign array value: b == [8, 10]. Index {b:1} is multikey here. Local key 8 (_id:7) matches
        // via element 8; local key 10 (_id:1, _id:6) matches via element 10. The dedup logic must ensure
        // this single foreign doc is returned at most once per local document even though two index
        // seeks (for distinct local keys) could each find it.
        {_id: 106, b: [8, 10]},
        // Nested value for the dotted-foreign-field case. Its top-level 'b' is an object, so
        // it never matches the scalar 'b' join used by every other case (it leaves 'expected'
        // untouched); but for foreignField "b.b1" it exposes b1 == 10, matching local keys 10.
        {_id: 107, b: {b1: 10}},
    ]),
);

// The expected result is independent of which index (if any) exists - results must always be
// complete. We anchor the test to these hard-coded expectations so the test fails even if both the
// indexed and non-indexed runs were wrong in the same way.
const expected = [
    {_id: 0, matched: [100]}, // b == 3
    {_id: 1, matched: [101, 104, 106]}, // b == 10, plus the array [8,10]
    {_id: 2, matched: [102]}, // b == 7
    {_id: 3, matched: [103]}, // b == 1
    {_id: 4, matched: [100, 101, 104, 106]}, // b == 3 or b == 10 (and [8,10])
    {_id: 5, matched: []}, // no foreign doc has b null/missing
    {_id: 6, matched: [101, 102, 104, 106]}, // b == 7 or b == 10 (and [8,10])
    {_id: 7, matched: [106]}, // 8 is an element of [8,10]; appears exactly once
];

// Partial index whose filter references only the foreign field. This is eligible for SBE DILJ.
assert.commandWorked(foreignColl.createIndex({b: 1}, {partialFilterExpression: {b: {$gt: 5}}}));

assert.eq(runLookup(pipeline), expected);
assert.eq(getExplainNode(pipeline, "EQ_LOOKUP").strategy, "DynamicIndexedLoopJoin");

assert.commandWorked(foreignColl.dropIndex({b: 1}));

// A compound partial index {b:1, c:1} with the same foreign-field-only filter is still eligible.
assert.commandWorked(
    foreignColl.createIndex({b: 1, c: 1}, {partialFilterExpression: {b: {$gt: 5}}}),
);

assert.eq(runLookup(pipeline), expected);
assert.eq(getExplainNode(pipeline, "EQ_LOOKUP").strategy, "DynamicIndexedLoopJoin");

assert.commandWorked(foreignColl.dropIndex({b: 1, c: 1}));

// A partial filter that is compound expression over multiple predicates, each referencing only
// the foreign field, so it is eligible for DILJ.
assert.commandWorked(
    foreignColl.createIndex(
        {b: 1},
        {partialFilterExpression: {$or: [{b: {$gte: 4}}, {b: {$lte: 2}}]}},
    ),
);

assert.eq(runLookup(pipeline), expected);
assert.eq(getExplainNode(pipeline, "EQ_LOOKUP").strategy, "DynamicIndexedLoopJoin");

assert.commandWorked(foreignColl.dropIndex({b: 1}));

// A non-partial index is preferred over partial index when both are eligible.
assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));
assert.commandWorked(foreignColl.createIndex({b: 1}, {partialFilterExpression: {b: {$gt: 5}}}));

assert.eq(runLookup(pipeline), expected);
assert.eq(getExplainNode(pipeline, "EQ_LOOKUP").strategy, "IndexedLoopJoin");
assert.eq(getExplainNode(pipeline, "IXSCAN").keyPattern, {b: 1, c: 1});

assert.commandWorked(foreignColl.dropIndex({b: 1}));
assert.commandWorked(foreignColl.dropIndex({b: 1, c: 1}));

// A partial index whose filter references a field other than the foreign field cannot be used by
// SBE DILJ (the per-key decision would be unsound), but results must still be correct.
assert.commandWorked(
    foreignColl.createIndex({b: 1}, {partialFilterExpression: {c: {$exists: true}}}),
);
assert.eq(runLookup(pipeline), expected);
assert.docEq(getExplainNode(pipeline, "$lookup"), pipeline[0]);

assert.commandWorked(foreignColl.dropIndex({b: 1}));

// A partial index with dotted foreign field cannot be used by SBE DILJ (due to complex filter
// generation), but results must still be correct.
const pipelineDotted = [
    {
        $lookup: {
            from: foreignColl.getName(),
            localField: "a",
            foreignField: "b.b1",
            as: "matched",
        },
    },
];
const expectedDotted = [
    {_id: 0, matched: []},
    {_id: 1, matched: [107]},
    {_id: 2, matched: []},
    {_id: 3, matched: []},
    {_id: 4, matched: [107]},
    {_id: 5, matched: []},
    {_id: 6, matched: [107]},
    {_id: 7, matched: []},
];
assert.commandWorked(
    foreignColl.createIndex({"b.b1": 1}, {partialFilterExpression: {"b.b1": {$gt: 5}}}),
);

assert.eq(runLookup(pipelineDotted), expectedDotted);
assert.docEq(getExplainNode(pipelineDotted, "$lookup"), pipelineDotted[0]);

assert.commandWorked(foreignColl.dropIndex({"b.b1": 1}));

// A partial index whose filter is not SBE compatible does not use DILJ
const geoLocal = db.geoLocal;
const geoForeign = db.geoForeign;

assert.commandWorked(
    geoLocal.insertMany([
        {_id: 0, a: {type: "Point", coordinates: [0, 0]}},
        {_id: 1, a: {type: "Point", coordinates: [40, 40]}},
    ]),
);
assert.commandWorked(
    geoForeign.insertMany([
        {_id: 200, b: {type: "Point", coordinates: [0, 0]}},
        {_id: 201, b: {type: "Point", coordinates: [40, 40]}},
    ]),
);
assert.commandWorked(
    geoForeign.createIndex(
        {b: "2dsphere"},
        {
            partialFilterExpression: {
                b: {
                    $geoWithin: {
                        $geometry: {
                            type: "Polygon",
                            coordinates: [
                                [
                                    [-1, -1],
                                    [-1, 1],
                                    [1, 1],
                                    [1, -1],
                                    [-1, -1],
                                ],
                            ],
                        },
                    },
                },
            },
        },
    ),
);
const pipelineGeo = [
    {$lookup: {from: geoForeign.getName(), localField: "a", foreignField: "b", as: "matched"}},
];
const expectedGeo = [
    {_id: 0, matched: [200]},
    {_id: 1, matched: [201]},
];

assert.eq(runLookup(pipelineGeo, geoLocal), expectedGeo);
assert.docEq(getExplainNode(pipelineGeo, "EQ_LOOKUP", geoLocal).strategy, "NestedLoopJoin");

MongoRunner.stopMongod(conn);
