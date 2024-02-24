/**
 * Tests that $expr with equality of $toHashedIndexKey to a NumberLong results in an IXSCAN plan
 * with a point bound. This is because we rewrite this structure to a $_internalEqHash expression
 * and generate a tight index bound.
 * @tags: [
 *   # explain doesn't support read concern
 *   assumes_read_concern_unchanged,
 *   requires_fcv_70,
 * ]
 */
import {
    getExecutionStages,
    getOptimizer,
    getPlanStages,
    isCollscan,
    isIxscan
} from "jstests/libs/analyze_plan.js";

const isCursorHintsToQuerySettings = TestData.isCursorHintsToQuerySettings || false;

const collName = jsTestName();
const coll = db.getCollection(collName);

/**
 * Helper function to get the index hash of given field in the row matching the given filterSpec.
 * @param {Collection} coll
 * @param {Object} filterSpec - Object representing an equality predicate. Expected to uniquely
 *     identify a single document.
 * @param {String} field - Name of the field which has a hashed index defined.
 * @param {Object} indexSpec - Object representing the index specification. Must have field as a
 *     hashed index.
 */
function getHash(coll, filterSpec, field, indexSpec) {
    const res = coll.aggregate(
        [
            {$match: filterSpec},
            {$set: {hashVal: {$let: {vars: {key: {$meta: "indexKey"}}, in : `$$key.${field}`}}}},
        ],
        {hint: indexSpec});
    return res.toArray()[0].hashVal;
}

/**
 * Given the explain output of a plan, assert that the plan used an index scan, the given index and
 * the number of keys examined.
 * @param {Object} explainPlan - Output of explain run with 'executionStats'.
 * @param {Object} expectedIndexSpec - The expected key pattern of the index scan.
 * @param {int} expectedKeysExamined - The expected number of keys in the index that were examined.
 */
function assertExplainIxscan(explainPlan, expectedIndexSpec, expectedKeysExamined = 1) {
    switch (getOptimizer(explainPlan)) {
        case "classic": {
            assert(isIxscan(db, explainPlan), explainPlan);
            break;
        }
        case "CQF": {
            // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
            // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
            assert(isCollscan(db, explainPlan));
            break;
        }
    }
    let execStages = getExecutionStages(explainPlan);
    execStages.forEach(execStage => {
        if (execStage.stage == "SHARDING_FILTER" && execStage.nReturned == 0) {
            return;
        }
        let ixscan = getPlanStages(execStage, "IXSCAN")[0];
        jsTestLog(ixscan);
        assert.eq(expectedIndexSpec, ixscan.keyPattern);
        assert.eq(expectedKeysExamined, ixscan.keysExamined);
    });
}

(function testSingleFieldHashedIndex() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 1, a: "foo"},
        {_id: 2, a: 123},
        {_id: 3, a: new Date()},
    ]));
    const indexSpec = {a: "hashed"};
    assert.commandWorked(coll.createIndex(indexSpec));

    const hash = getHash(coll, {a: "foo"}, "a", indexSpec);
    const testQuery = {$expr: {$eq: [{$toHashedIndexKey: '$a'}, hash]}};
    const explainPlan = coll.find(testQuery).hint(indexSpec).explain("executionStats");

    assertExplainIxscan(explainPlan, indexSpec);
})();

(function testCompoundIndexFirstFieldHashed() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 1, a: 10, b: "foo"},
        {_id: 2, a: 20, b: "bar"},
        {_id: 3, a: 30, b: "baz"},
    ]));
    const indexSpec = {a: "hashed", b: 1};
    assert.commandWorked(coll.createIndex(indexSpec));

    const hash = getHash(coll, {a: 10}, "a", indexSpec);
    const testQuery = {
        $and: [
            {$expr: {$eq: [{$toHashedIndexKey: '$a'}, hash]}},
            {$expr: {$eq: ['$b', "foo"]}},
        ]
    };
    const explainPlan =
        coll.explain("executionStats").aggregate([{$match: testQuery}], {hint: indexSpec});

    assertExplainIxscan(explainPlan, indexSpec);
})();

(function testCompoundIndexLastFieldHashed() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 1, a: 10, b: "foo"},
        {_id: 2, a: 20, b: "bar"},
        {_id: 3, a: 30, b: "baz"},
    ]));
    const indexSpec = {b: 1, a: "hashed"};
    assert.commandWorked(coll.createIndex(indexSpec));

    const hash = getHash(coll, {a: 10}, "a", indexSpec);
    const testQuery = {
        $and: [
            {$expr: {$eq: [{$toHashedIndexKey: '$a'}, hash]}},
            {$expr: {$eq: ['$b', "foo"]}},
        ]
    };
    const explainPlan =
        coll.explain("executionStats").aggregate([{$match: testQuery}], {hint: indexSpec});

    assertExplainIxscan(explainPlan, indexSpec);
})();

(function testCompoundIndexMiddleFieldHashed() {
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 1, a: 10, b: "foo", c: "fred"},
        {_id: 2, a: 20, b: "bar", c: "waldo"},
        {_id: 3, a: 30, b: "baz", c: "thud"},
    ]));
    const indexSpec = {b: 1, a: "hashed", c: 1};
    assert.commandWorked(coll.createIndex(indexSpec));

    const hash = getHash(coll, {a: 10}, "a", indexSpec);
    const testQuery = {
        $and: [
            {$expr: {$eq: [{$toHashedIndexKey: '$a'}, hash]}},
            {$expr: {$eq: ['$b', "foo"]}},
            {$expr: {$eq: ['$c', "fred"]}},
        ]
    };
    const explainPlan =
        coll.explain("executionStats").aggregate([{$match: testQuery}], {hint: indexSpec});

    assertExplainIxscan(explainPlan, indexSpec);
})();

(function testNoHashedIndex() {
    // This test case is using a bad index. Query settings will not apply bad indexes and therefore
    // this test case should not run in cursor hints to query settings suite.
    if (isCursorHintsToQuerySettings) {
        return;
    }

    coll.drop();
    coll.dropIndexes();
    assert.commandWorked(coll.insert([
        {_id: 1, a: 10},
        {_id: 2, a: 20},
        {_id: 3, a: 30},
    ]));
    const hashIndexSpec = {a: 1};
    assert.commandWorked(coll.createIndex(hashIndexSpec));
    const hash = getHash(coll, {a: 10}, "a", hashIndexSpec);

    coll.dropIndexes();

    const indexSpec = {a: 1};
    assert.commandWorked(coll.createIndex(indexSpec));

    const testQuery = {$expr: {$eq: [{$toHashedIndexKey: '$a'}, hash]}};
    // Note that this query is hinting a bad index. Since it is not hashed, the plan degenerates
    // into a IXSCAN + FETCH, whereas if we didn't hint the index, the plan enumerator wouldn't have
    // even considered the '{a: 1}' index as eligible.
    const explainPlan =
        coll.explain("executionStats").aggregate([{$match: testQuery}], {hint: indexSpec});

    // We couldn't create a tight bound for the index scan as the index is not hashed.
    assertExplainIxscan(explainPlan, indexSpec, 3 /* keyExamined */);
})();
