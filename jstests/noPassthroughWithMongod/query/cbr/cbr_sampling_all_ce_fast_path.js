/**
 * Integration tests for the $all fast path in the sampling cardinality estimator.
 *
 * The fast path is triggered when a query is an AND of EqualityMatchExpressions all sharing the
 * same field path (the shape produced by $all). It pre-sorts the required values and uses
 * std::includes() to check each sample document once instead of re-scanning the array once per value.
 *
 */

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {getCBRConfig, setCBRConfig} from "jstests/libs/query/cbr_utils.js";

// TODO SERVER-92589: Remove this exemption.
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// ---------------------------------------------------------------------------
// Data setup
// ---------------------------------------------------------------------------
const allValues = [1, 2, 3, 4, 5];
assert.commandWorked(
    coll.insertMany([
        // regular / duplicate-value test docs
        {_id: 0, a: [...allValues, 11]}, // all 5 required values plus an extra
        {_id: 1, a: [0, ...allValues]}, // all 5 required values plus an extra
        {_id: 2, a: [...allValues]}, // exactly the 5 required values
        {_id: 3, a: allValues.slice(0, -1)}, // missing value 5
        {_id: 4, a: allValues.slice(1)}, // missing value 1
        {_id: 5, b: "not a field"},
        // dotted-path test docs
        {_id: 6, a: [{b: 1}, {b: 2}]}, // matched by regular $all, missed by fast path
        {_id: 7, a: {b: [1, 2]}}, // matched by regular $all and fast path
        {_id: 8, a: [{b: 1}, {b: 3}]}, // not matched (missing b:2)
    ]),
);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Returns the cardinality estimate from the winning plan for the given query.
 */
function getCE(query) {
    const explain = coll.find(query).explain();
    return getWinningPlanFromExplain(explain).cardinalityEstimate;
}

/**
 * Returns the actual numbter of documents that match the query by executing it.
 */
function getActualCount(query) {
    return coll.find(query).itcount();
}

// ---------------------------------------------------------------------------
// Enable sampling CE with a full sequential scan so that estimates are
// deterministic and the full collection is used as the sample. Force use of classic engine to ensure CBR will run.
// ---------------------------------------------------------------------------
const originalCBRParamValues = getCBRConfig(db);
setCBRConfig(db, {
    featureFlagCostBasedRanker: true,
    internalQueryCBRCEMode: "samplingCE",
});

const originalSamplingBySequentialScanValue = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
).was;

const originalFrameworkControlValue = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
).was;

try {
    // -----------------------------------------------------------------------
    // Test 1 – Simple $all on a top-level array field
    // -----------------------------------------------------------------------
    // _id 0, 1, 2 each contain all of [1..5] → 3 matching docs.
    const simpleQuery = {a: {$all: allValues}};
    const simpleExpected = getActualCount(simpleQuery);
    const simpleQueryCE = getCE(simpleQuery);
    assert.eq(
        simpleExpected,
        simpleQueryCE,
        `regular $all: CE ${simpleQueryCE} should equal actual count ${simpleExpected}`,
    );

    // -----------------------------------------------------------------------
    // Test 2 – Duplicate values in the $all list
    // -----------------------------------------------------------------------
    // Duplicates in the $all list are ignored, so this behavior needs to be replicated by the fast path.
    const dupQuery = {a: {$all: [1, 1, 2]}};
    const deduplicatedQuery = {a: {$all: [1, 2]}};
    const dupExpected = getActualCount(deduplicatedQuery);

    const dupCE = getCE(dupQuery);
    assert.eq(dupExpected, dupCE, `duplicate $all values: CE ${dupCE} should match non-duplicate CE ${dupExpected}`);

    // -----------------------------------------------------------------------
    // Test 3 – Dotted-path $all
    // -----------------------------------------------------------------------
    // {"a.b": {$all: [1, 2]}} matches:
    //   _id 6: {a: [{b:1},{b:2}]}
    //   _id 7: {a: {b: [1, 2]}}
    // → expected CE = 2
    //
    // The fast path cannot support dotted paths, so we check here that the generic CE code is
    // running and estimating cardinality correctly still.

    const dottedQuery = {"a.b": {$all: [1, 2]}};
    const dottedCE = getCE(dottedQuery);

    // Both matching documents are invisible to the fast path since it expects top level field names, so CE would be 0.
    // Instead we disallow the fast path for queries with dotted paths, so we should get the correct CE of 2 here.
    assert.eq(2, dottedCE, `dotted-path $all: Expected cardinality: 2, got estimated cardinality: ${dottedCE}`);
} finally {
    // Restore server parameter values
    setCBRConfig(db, originalCBRParamValues);
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQuerySamplingBySequentialScan: originalSamplingBySequentialScanValue,
            internalQueryFrameworkControl: originalFrameworkControlValue,
        }),
    );
}
