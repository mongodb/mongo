/**
 * Tests that an aggregation pipeline with stages only allowed with 'trySbeEngine' runs correctly
 * when wrapped with a $group, or $lookup. This makes the query use SBE.
 * @tags: [
 *    assumes_unsharded_collection,
 *    assumes_against_mongod_not_mongos,
 *    not_allowed_with_signed_security_token,
 *    does_not_support_causal_consistency,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    tenant_migration_incompatible,
 *    # Explain for the aggregate command cannot run within a multi-document transaction.
 *    does_not_support_transactions,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    requires_fcv_80,
 * ]
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {getEngine} from "jstests/libs/analyze_plan.js";

function buildErrorString(found, expected) {
    return "Expected:\n" + tojson(expected) + "\nGot:\n" + tojson(found);
}

function makeExpectedDocs(lowerBound, upperBound, isProject = false) {
    let expectedDocsGroup = [];
    let expectedDocsLookup = [];
    for (let i = lowerBound; i < upperBound; i++) {
        expectedDocsGroup.push({_id: i});
        // We project on {x: 1} so 'y' will not be present in the result.
        if (isProject) {
            expectedDocsLookup.push({_id: i, x: i, xx: []});
        } else {
            expectedDocsLookup.push({_id: i, x: i, y: i, xx: []});
        }
    }
    return [expectedDocsGroup, expectedDocsLookup];
}

function runAndVerifyQuery(coll, pipeline, [expectedDocsGroup, expectedDocsLookup]) {
    // Run the query and explain.
    pipeline.push({$group: {_id: "$x"}});
    let res = coll.aggregate(pipeline);
    assert(arrayEq(res.toArray(), expectedDocsGroup),
           buildErrorString(res.toArray(), expectedDocsGroup));
    let explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    assert.eq(getEngine(explain), "sbe", tojson(explain));

    pipeline.pop();
    pipeline.push({$lookup: {from: coll2Name, localField: "y", foreignField: "z", as: "xx"}});
    res = coll.aggregate(pipeline);
    assert(arrayEq(res.toArray(), expectedDocsLookup),
           buildErrorString(res.toArray(), expectedDocsLookup));
    explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    assert.eq(getEngine(explain), "sbe", tojson(explain));
}

// Runs the query and verifies properties of the result and explain. If 'withIndex' is true, for
// some queries, we use a distinct scan, which is only in the classic engine.
function runQueries(coll, withIndex = false) {
    // $limit queries.
    runAndVerifyQuery(coll, [{$limit: 5}], makeExpectedDocs(0, 5));

    // $skip queries.
    runAndVerifyQuery(coll, [{$skip: 5}], makeExpectedDocs(5, 100));

    // $limit + $skip queries.
    runAndVerifyQuery(coll, [{$limit: 50}, {$skip: 10}], makeExpectedDocs(10, 50));
    runAndVerifyQuery(coll, [{$skip: 10}, {$limit: 5}], makeExpectedDocs(10, 15));

    // $sort + $limit + $skip queries.
    runAndVerifyQuery(coll, [{$sort: {x: 1}}, {$skip: 10}, {$limit: 20}], makeExpectedDocs(10, 30));
    runAndVerifyQuery(coll, [{$sort: {x: 1}}, {$limit: 40}, {$skip: 30}], makeExpectedDocs(30, 40));

    // Mixed queries.
    runAndVerifyQuery(
        coll,
        [{$match: {x: {$lte: 20}}}, {$sort: {x: 1}}, {$skip: 8}, {$limit: 5}, {$project: {x: 1}}],
        makeExpectedDocs(8, 13, true /*isProject*/));

    if (!withIndex) {
        // $sort queries.
        runAndVerifyQuery(coll, [{$sort: {x: 1}}], makeExpectedDocs(0, 100));
        runAndVerifyQuery(coll, [{$sort: {y: 1}}], makeExpectedDocs(0, 100));

        // $project queries.
        runAndVerifyQuery(coll, [{$project: {x: 1}}], makeExpectedDocs(0, 100, true /*isProject*/));

        // $match queries.
        runAndVerifyQuery(coll, [{$match: {x: 4}}], makeExpectedDocs(4, 5));
    }
}

function testIndexed(coll) {
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));
    runQueries(coll, true /* withIndex */);
}

let originalParamValue;
const collName = jsTestName();
const coll2Name = jsTestName() + "2";
let coll = db.getCollection(collName);
let coll2 = db.getCollection(coll2Name);
coll.drop();
coll2.drop();

try {
    originalParamValue = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeRestricted"}));

    const docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({_id: i, x: i, y: i});
    }

    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll2.insert({z: 100}));
    runQueries(coll);
    testIndexed(coll);
} finally {
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: originalParamValue.internalQueryFrameworkControl
    }));
}
