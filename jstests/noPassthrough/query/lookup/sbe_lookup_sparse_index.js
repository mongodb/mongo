/**
 * Tests that SBE can use a sparse index on the foreign side of a $lookup and still return correct
 * results. A sparse index omits documents that are missing the indexed field, so for local keys
 * that are null or missing the index cannot be used (it would drop foreign documents that are
 * missing the field). SBE handles this with the dynamic indexed loop join (DILJ): per local key it
 * seeks the sparse index when the key is a real, non-null value and falls back to a collection scan
 * when the key is null or missing.
 *
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: "trySbeEngine"}});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTestName());

if (!checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping test because SBE is disabled");
    MongoRunner.stopMongod(conn);
    quit();
}

const local = db.local;
const foreign = db.foreign;

// Local doc with a value, a missing field, and an explicit null.
const localDocs = [{_id: 1, a: "apple"}, {_id: 2}, {_id: 3, a: null}];
// Foreign docs: one with a value, one with an explicit null, one missing "a" entirely (_id:12 is
// NOT in a sparse index on "a").
const foreignDocs = [
    {_id: 10, a: "apple", details: "red fruit"},
    {_id: 11, a: null, details: "unknown value"},
    {_id: 12, details: "missing 'a' field entirely"},
];

// MQL semantics: a missing or null local "a" matches BOTH the null foreign doc (_id:11) and the
// missing-field foreign doc (_id:12).
const expected = [
    {_id: 1, matchedIds: [10]},
    {_id: 2, matchedIds: [11, 12]},
    {_id: 3, matchedIds: [11, 12]},
];

const pipeline = [
    {$lookup: {from: foreign.getName(), localField: "a", foreignField: "a", as: "m"}},
    {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
];

function reset(foreignIndexSpecs) {
    local.drop();
    foreign.drop();
    assert.commandWorked(local.insert(localDocs));
    assert.commandWorked(foreign.insert(foreignDocs));
    for (const spec of foreignIndexSpecs) {
        assert.commandWorked(foreign.createIndex(spec.key, spec.options));
    }
}

// Runs the pipeline and asserts the chosen SBE strategy and correct results.
function runCase({label, indexes, aggOptions, expectedStrategy}) {
    reset(indexes);

    const planExplain = local.explain().aggregate(pipeline, aggOptions);
    assert.eq(
        "sbe",
        getEngine(planExplain),
        `${label}: expected SBE pushdown: ${tojson(planExplain)}`,
    );
    const eqLookupNodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(1, eqLookupNodes.length, `${label}: expected one EQ_LOOKUP: ${tojson(planExplain)}`);
    assert.eq(
        expectedStrategy,
        eqLookupNodes[0].strategy,
        `${label}: unexpected strategy: ${tojson(planExplain)}`,
    );

    const results = local.aggregate(pipeline, aggOptions).toArray();
    assertArrayEq({
        actual: results,
        expected,
        extraErrorMsg: `${label}: incorrect $lookup results`,
    });
}

// Case 1: sparse index, HashJoin disabled (allowDiskUse:false) -> DILJ.
runCase({
    label: "sparse, DILJ",
    indexes: [{key: {a: 1}, options: {sparse: true}}],
    aggOptions: {allowDiskUse: false},
    expectedStrategy: "DynamicIndexedLoopJoin",
});

// Case 2: sparse index with HashJoin allowed -> HashJoin (also correct; it scans the foreign side).
runCase({
    label: "sparse, HashJoin",
    indexes: [{key: {a: 1}, options: {sparse: true}}],
    aggOptions: {allowDiskUse: true},
    expectedStrategy: "HashJoin",
});

// Case 3: a sparse HASHED index is also handled correctly via DILJ.
runCase({
    label: "sparse hashed, DILJ",
    indexes: [{key: {a: "hashed"}, options: {sparse: true}}],
    aggOptions: {allowDiskUse: false},
    expectedStrategy: "DynamicIndexedLoopJoin",
});

// Case 4: when a non-sparse index is also available, it is preferred and the faster INLJ is kept.
runCase({
    label: "non-sparse preferred",
    indexes: [
        {key: {a: 1}, options: {}},
        {key: {a: 1, c: 1}, options: {sparse: true}},
    ],
    aggOptions: {allowDiskUse: false},
    expectedStrategy: "IndexedLoopJoin",
});

// Case 5: a sparse index whose collation is ALSO incompatible with the query. Both runtime guards
// must apply together via DILJ: collation-sensitive (string) local keys AND null/missing local keys
// fall back to the collection scan, while collation-independent non-null keys (e.g. numbers) still
// seek the index.
{
    const lc = db.combo_local;
    const fc = db.combo_foreign;
    lc.drop();
    fc.drop();
    assert.commandWorked(
        lc.insert([
            {_id: 1, a: "apple"}, // string -> collation guard -> scan
            {_id: 2, a: 7}, // number -> collation-independent and non-null -> index seek
            {_id: 3}, // missing -> sparse guard -> scan
            {_id: 4, a: null}, // null -> sparse guard -> scan
        ]),
    );
    assert.commandWorked(
        fc.insert([
            {_id: 10, a: "Apple"}, // differs from local "apple" only by case
            {_id: 11, a: "apple"},
            {_id: 12, a: 7},
            {_id: 13, a: null},
            {_id: 14}, // missing 'a' -> absent from the sparse index
        ]),
    );
    // Sparse index with a case-insensitive collation: incompatible with the (simple) query
    // collation, so DILJ must apply BOTH the collation type guard and the sparse null guard.
    assert.commandWorked(
        fc.createIndex({a: 1}, {sparse: true, collation: {locale: "en_US", strength: 2}}),
    );

    const comboPipeline = [
        {$lookup: {from: fc.getName(), localField: "a", foreignField: "a", as: "m"}},
        {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
    ];
    const aggOptions = {allowDiskUse: false}; // force DILJ

    const planExplain = lc.explain().aggregate(comboPipeline, aggOptions);
    assert.eq("sbe", getEngine(planExplain), `combo: expected SBE: ${tojson(planExplain)}`);
    const nodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(1, nodes.length, `combo: expected one EQ_LOOKUP: ${tojson(planExplain)}`);
    assert.eq("DynamicIndexedLoopJoin", nodes[0].strategy, `combo: ${tojson(planExplain)}`);

    // Expected under the simple (case-sensitive) query collation:
    //   _id:1 "apple" -> only the exact lowercase "apple" (_id:11), NOT "Apple": proves the
    //                    case-insensitive index was NOT used for this string key (collation guard).
    //   _id:2 7       -> _id:12, found via the index seek.
    //   _id:3 missing -> null doc (_id:13) AND missing-field doc (_id:14, absent from the sparse
    //   _id:4 null    -> index): proves the sparse guard forced a scan for these keys.
    const comboExpected = [
        {_id: 1, matchedIds: [11]},
        {_id: 2, matchedIds: [12]},
        {_id: 3, matchedIds: [13, 14]},
        {_id: 4, matchedIds: [13, 14]},
    ];
    const comboResults = lc.aggregate(comboPipeline, aggOptions).toArray();
    assertArrayEq({
        actual: comboResults,
        expected: comboExpected,
        extraErrorMsg: "combo (sparse + incompatible collation) incorrect results",
    });
}

MongoRunner.stopMongod(conn);
