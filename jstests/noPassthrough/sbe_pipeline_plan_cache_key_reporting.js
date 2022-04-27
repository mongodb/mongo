/**
 * Confirms that 'planCacheKey' and 'queryHash' are correctly reported when the query has $lookup
 * and $query stages with enabled and disabled SBE Plan Cache.
 */

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const databaseName = "pipeline_plan_cache_key_reporting";

function isSBEEnabled() {
    const conn = MongoRunner.runMongod({});
    try {
        const db = conn.getDB(databaseName);
        return checkSBEEnabled(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

if (!isSBEEnabled()) {
    jsTest.log("Skipping test because SBE is not enabled.");
    return;
}

/**
 * Driver function that creates mongod instances with specified parameters and run the given test
 * cases.
 * @param {*} params to be passed to mongod in format like { setParameter:
 *     "featureFlagSbePlanCache=true"}
 * @param {*} testCases a list of test cases where each test case is an object with 'setup(db)' and
 *     'run(db, assertMessage)' functions.
 * @returns results from 'testCase.run(db, assertMessage)'
 */
function runTests(params, testCases) {
    let results = [];
    const conn = MongoRunner.runMongod(params);
    const db = conn.getDB(databaseName);

    const assertMessage = `${tojson(params)}`;
    try {
        for (let testCase of testCases) {
            testCase.setup(db);
            results.push(testCase.run(db, assertMessage));
        }
    } finally {
        MongoRunner.stopMongod(conn);
    }
    return results;
}

/**
 * This function validates given explain and return and object with extracted and validated
 * PlanCacheKey and QueryHash.
 * @returns {planCacheKey, queryHash, explain}
 */
function processAndValidateExplain(explain, assertMessage) {
    assert.neq(explain, null);
    assert.eq(explain.explainVersion,
              "2",
              `[${assertMessage}] invalid explain version ${tojson(explain)}`);

    const planCacheKey = explain.queryPlanner.planCacheKey;
    validateKey(planCacheKey, `[${assertMessage}] Invalid planCacheKey: ${tojson(explain)}`);

    const queryHash = explain.queryPlanner.queryHash;
    validateKey(queryHash, `[${assertMessage}] Invalid queryHash: ${tojson(explain)}`);

    return {planCacheKey, queryHash, explain};
}

/**
 * Validates given 'key' (PlanCacheKey or QueryHash).
 */
function validateKey(key, assertMessage) {
    assert.eq(typeof key, "string", assertMessage);
    assert.gt(key.length, 0, assertMessage);
}

// 1. Create test cases for $lookup and $group stages.
const lookupTestCase = {
    setup: db => {
        db.coll.drop();
        assert.commandWorked(db.coll.createIndexes([{a: 1}, {a: 1, b: 1}]));

        db.lookupColl.drop();
        assert.commandWorked(db.lookupColl.createIndex({b: 1}));
    },

    run: (db, assertMessage) => {
        const pipeline = [
            {$lookup: {from: db.lookupColl.getName(), localField: "a", foreignField: "b", as: "w"}}
        ];
        const explain = db.coll.explain().aggregate(pipeline);
        return processAndValidateExplain(explain, assertMessage);
    },
};

const groupTestCase = {
    setup: db => {
        db.coll.drop();
        assert.commandWorked(db.coll.insertOne({a: 1}));
    },

    run: (db, assertMessage) => {
        const pipeline = [{
            $group: {
                _id: "$b",
            }
        }];
        const explain = db.coll.explain().aggregate(pipeline);
        return processAndValidateExplain(explain, assertMessage);
    },
};

const testCases = [lookupTestCase, groupTestCase];

// 2. Run the test cases with SBE Plan Cache Enabled.
const sbeParams = {
    setParameter: "featureFlagSbePlanCache=true"
};
const sbeKeys = runTests(sbeParams, testCases);
assert.eq(testCases.length, sbeKeys.length);

// 3. Run the test cases with SBE Plan Cache disabled.
const classicParams = {
    setParameter: "featureFlagSbePlanCache=false"
};
const classicKeys = runTests(classicParams, testCases);
assert.eq(testCases.length, classicKeys.length);

// 4. Validate that PlanCacheKeys and QueryHash are equal. They should be different once
// SERVER-61507 is completed.
for (let i = 0; i < sbeKeys.length; ++i) {
    const sbe = sbeKeys[i];
    const classic = classicKeys[i];

    const message = `sbe=${tojson(sbe.explain)}, classic=${tojson(classic.explain)}`;
    assert.eq(sbe.planCacheKey, classic.planCacheKey, message);
    assert.eq(sbe.queryHash, classic.queryHash, message);
}
})();
