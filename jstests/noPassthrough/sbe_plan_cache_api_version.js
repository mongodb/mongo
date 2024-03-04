/**
 * Validates the behaviour of the the SBE plan cache when the API version was provided to the
 * aggregate command.
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_incompatible,
 * ]
 */

import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("plan_cache_api_version");
const coll = db.coll;
coll.drop();

const isSBEEnabled = checkSbeFullyEnabled(db);

assert.commandWorked(coll.insert([{a: 1, b: 1}, {a: 2, b: 2}]));

// Runs the given pipeline with the specified options and returns its plan cache key.
function runPipeline(pipeline, options, explainOptions = {}) {
    const command = Object.assign({aggregate: coll.getName(), pipeline, cursor: {}}, options);
    const result = coll.runCommand(command);
    assert.commandWorked(result);
    assert.eq(result.cursor.firstBatch.length, 1, result.cursor.firstBatch);

    const explain = coll.runCommand(Object.assign({explain: command}, explainOptions));
    assert.commandWorked(explain);
    assert.neq(explain, null);
    assert.eq(explain.explainVersion, isSBEEnabled ? "2" : "1", explain);
    assert.neq(explain.queryPlanner.planCacheKey, null, explain);
    return explain.queryPlanner.planCacheKey;
}

// Runs the given 'pipeline' with the API version and returns its plan cache key.
function runPipelineWithApiVersion(pipeline) {
    const options = {apiVersion: '1'};
    return runPipeline(pipeline, options, options);
}

// Runs the given 'pipeline' with the API version and 'apiStrict: true' and returns its plan cache
// key.
function runPipelineWithApiStrict(pipeline) {
    const options = {apiVersion: '1', apiStrict: true};
    return runPipeline(pipeline, options, options);
}

// Asserts that a plan cache entry for the given 'cacheKey' exists in the plan cache and has
// certain properties set as per provided 'properties' argument.
function assertPlanCacheEntryExists(cacheKey, properties = {}) {
    const entries =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: cacheKey}}]).toArray();
    assert.eq(entries.length, 1, entries);
    const entry = entries[0];

    if (isSBEEnabled) {
        // The version:"2" field indicates that this is an SBE plan cache entry.
        assert.eq(entry.version, "2", entry);
        assert.eq(entry.isActive, properties.isActive, entry);
        assert.eq(entry.isPinned, properties.isPinned, entry);
    } else {
        // The version:"1" field indicates that this is an classic plan cache entry.
        assert.eq(entry.version, "1", entry);
        assert.eq(entry.isActive, properties.isActive, entry);
    }
}

const pipeline = [{$match: {a: 1, b: 1}}];

// Run a set of testcases where each testcase defines a set of indexes on the collection and
// executes the above pipeline with and without the API strict flag. Assert that the plan cache
// keys for each of the two queries are different and two different plan cache entries have been
// created.

const sbeEngineTestcases = [
    {
        withApiVersion: {isActive: true, isPinned: true},
        withApiStrict: {isActive: true, isPinned: true},
        indexSpecs: []
    },
    {
        withApiVersion: {isActive: true, isPinned: true},
        withApiStrict: {isActive: true, isPinned: true},
        indexSpecs: [{keyPattern: {a: 1}, options: {name: "a_1"}}]
    },
    {
        withApiVersion: {isActive: false, isPinned: false},
        withApiStrict: {isActive: true, isPinned: true},
        indexSpecs: [
            {keyPattern: {a: 1}, options: {name: "a_1"}},
            {keyPattern: {a: 1}, options: {name: "a_1_sparse", sparse: true}}
        ]
    }
];

const classicEngineTestcases = [
    {
        withApiVersion: {isActive: false},
        withApiStrict: {isActive: false},
        indexSpecs: [
            {keyPattern: {a: 1}, options: {name: "a_1"}},
            {keyPattern: {b: 1}, options: {name: "b_1"}}
        ]
    },
    {
        withApiVersion: {isActive: false},
        withApiStrict: {isActive: false},
        indexSpecs: [
            {keyPattern: {a: 1}, options: {name: "a_1"}},
            {keyPattern: {a: 1}, options: {name: "a_1_sparse", sparse: true}},
            {keyPattern: {b: 1}, options: {name: "b_1"}}
        ]
    }
];

const testcases = isSBEEnabled ? sbeEngineTestcases : classicEngineTestcases;
for (const testcase of testcases) {
    [true, false].forEach((runWithApiStrictFirst) => {
        assert.commandWorked(coll.dropIndexes());

        for (const indexSpec of testcase.indexSpecs) {
            assert.commandWorked(coll.createIndex(indexSpec.keyPattern, indexSpec.options));
        }

        let planCacheKeyWithApiVersion;
        let planCacheKeyWithApiStrict;

        if (runWithApiStrictFirst) {
            planCacheKeyWithApiStrict = runPipelineWithApiStrict(pipeline);
            planCacheKeyWithApiVersion = runPipelineWithApiVersion(pipeline);
        } else {
            planCacheKeyWithApiVersion = runPipelineWithApiVersion(pipeline);
            planCacheKeyWithApiStrict = runPipelineWithApiStrict(pipeline);
        }

        assert.neq(planCacheKeyWithApiVersion, planCacheKeyWithApiStrict);
        assertPlanCacheEntryExists(planCacheKeyWithApiVersion, testcase.withApiVersion);
        assertPlanCacheEntryExists(planCacheKeyWithApiStrict, testcase.withApiStrict);
    });
}

MongoRunner.stopMongod(conn);
