/**
 * Tests that a compound wildcard index can be used to support non-blocking sorts via index scan.
 * @tags: [
 *   assumes_balancer_off,
 *   # We may choose a different plan if other indexes are created, which would break the test.
 *   assumes_no_implicit_index_creation,
 *   assumes_read_concern_local,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For getWinningPlan(), getPlanStages().
load("jstests/libs/fixture_helpers.js");      // For numberOfShardsForCollection().

const coll = db.compound_wildcard_sort;
coll.drop();

function flip(keyPattern) {
    const out = {};
    for (const key of Object.keys(keyPattern)) {
        out[key] = keyPattern[key] * -1;
    }
    return out;
}

function replaceFieldWith(keyPattern, fieldToReplace, keys) {
    let newSort = {};
    for (const [field, sortDir] of Object.entries(keyPattern)) {
        if (field == fieldToReplace) {
            for (const key of keys) {
                newSort[key] = sortDir;
            }
        } else {
            newSort[field] = sortDir;
        }
    }
    return newSort;
}

function getExplain({pred, sort, proj, natural}) {
    if (natural) {
        return assert.commandWorked(coll.find(pred, proj).sort(sort).hint({$natural: 1}).explain());
    }
    return assert.commandWorked(coll.find(pred, proj).sort(sort).explain());
}

function validateExplain({pred, sort, proj, natural, blockingSort}) {
    const explain = getExplain({pred, sort, proj, natural});
    const plan = getWinningPlan(explain.queryPlanner);
    const ixScans = getPlanStages(plan, "IXSCAN");
    const collScans = getPlanStages(plan, "COLLSCAN");
    const sorts = getPlanStages(plan, "SORT");

    if (blockingSort) {
        assert.eq(sorts.length, FixtureHelpers.numberOfShardsForCollection(coll), explain);
        // A blocking sort may or may not use the index, so we don't check the length of 'ixScans'.
    } else {
        assert.eq(sorts.length, 0, explain);
        assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll), explain);
        assert.eq(collScans.length, 0, explain);
    }
}

function runSortTest({pred, sort, proj, blockingSort}) {
    // Ensure query uses expected plan.
    validateExplain({pred, sort, proj, blockingSort});

    // Ensure $natural sort uses blocking sort.
    validateExplain({pred, proj, sort, natural: true, blockingSort: true});

    // Compare query results against $natural plan.
    const actual = coll.find(pred, proj).sort(sort).toArray();
    const expected = coll.find(pred, proj).sort(sort).hint({$natural: 1}).toArray();
    assertArrayEq({actual, expected});
}

function initializeDocs() {
    coll.drop();
    const strs = ["abc", "def", "ghi", "jkl", "mnopqrstuvwxyz"];
    for (let i = 0; i < 3; i++) {
        const pre = i;
        const post = -i;
        const num1 = i + 42;
        const num2 = 44 - i;
        const str1 = strs[i];
        const str2 = strs[4 - i];
        const wild = {str1, num1};
        const docs = [
            {pre, wild, str2, num2, post},
            {pre, wild, str2: "", num2, post},
            {/* 'pre' missing */ wild, str2, num2, post},
            {pre, /* 'wild' missing */ str2, num2, post},
            {pre, wild: {/* 'str1', 'num1' missing */}, str2, num2, post},
            {pre, wild: {str1: "", num1}, str2, num2, post},
            {pre, wild, /* 'str2', 'num2' missing */ post},
        ];
        assert.commandWorked(coll.insert(docs));
    }
}

function getWildcardIndexesAndOptionsFor(w, isNested) {
    // Test that restriction for descending single wildcard index was removed.
    const indexesAndOptions = [[{[w]: -1}, {}]];

    // Test cases with exclusions.
    if (!isNested) {
        return indexesAndOptions.concat([
            [{pre: 1, [w]: 1}, {wildcardProjection: {pre: 0}}],
            [{[w]: 1, post: -1}, {wildcardProjection: {post: 0}}],
            [{pre: 1, [w]: 1, post: 1}, {wildcardProjection: {pre: 0, post: 0}}],
        ]);
    }

    // Test cases with nested wildcard path.
    return indexesAndOptions.concat([
        [{pre: 1, [w]: -1}, undefined],
        [{[w]: -1, post: -1}, undefined],
        [{pre: -1, [w]: 1, post: -1}, undefined],
    ]);
}

function sortToProj(keyPattern) {
    let proj = {};
    for (const k of Object.keys(keyPattern)) {
        proj[k] = 1;
    }
    proj._id = 0;
    return proj;
}

function makeIndexCompatPred(index, pred) {
    if (index.hasOwnProperty("pre")) {
        return Object.assign({pre: {$gt: 0}}, pred);
    }
    return pred;
}

function runSortTestForWildcardField({index, sort, wildFieldPred}) {
    const proj = sortToProj(sort);

    // Sort on whole collection results in blocking sort.
    runSortTest({sort, proj, blockingSort: true});

    // Sort with filter + appropriate projection can leverage index.
    const pred = makeIndexCompatPred(index, wildFieldPred);
    runSortTest({pred, sort, proj, blockingSort: false});

    if (index.hasOwnProperty("post")) {
        runSortTest({pred: Object.assign(pred, {post: {$lt: 1}}), sort, proj, blockingSort: false});
    }
}

function getValidKeyPatternPrefixesForSort(keyPattern) {
    let valid = [keyPattern, flip(keyPattern)];
    if (keyPattern.hasOwnProperty("pre")) {
        valid = valid.concat({pre: 1}, {pre: -1});
    }
    if (keyPattern.hasOwnProperty("post")) {
        const updatedKP = Object.assign({}, keyPattern);
        delete updatedKP.post;
        valid = valid.concat(updatedKP, flip(updatedKP));
    }
    return valid;
}

function testIndexesForWildcardField(wildcardField, subFields) {
    const isNested = wildcardField.includes(".");
    const indexesAndOptions = getWildcardIndexesAndOptionsFor(wildcardField, isNested);

    for (const [keyPattern, options] of indexesAndOptions) {
        assert.commandWorked(coll.createIndex(keyPattern, options));

        const valid = getValidKeyPatternPrefixesForSort(keyPattern);
        for (const kp of valid) {
            // CWI with regular prefix cannot provide blocking sort for sort orders containing the
            // wildcard field.
            if (!keyPattern.hasOwnProperty('pre')) {
                {
                    // Test sort on compound fields + first wildcard field (number).
                    const sort = replaceFieldWith(kp, wildcardField, [subFields[0]]);
                    const wildFieldPred = {[subFields[0]]: {$lte: 43}};
                    runSortTestForWildcardField({index: keyPattern, sort, wildFieldPred});
                }

                {
                    // Test sort on compound fields + second wildcard field (string).
                    const sort = replaceFieldWith(kp, wildcardField, [subFields[1]]);
                    const wildFieldPred = {[subFields[1]]: {$gt: ""}};
                    runSortTestForWildcardField({index: keyPattern, sort, wildFieldPred});
                }
            }

            {
                const sort = replaceFieldWith(kp, wildcardField, subFields);
                const proj = sortToProj(sort);
                const wildFieldPred = {[subFields[0]]: {$gt: ""}, [subFields[1]]: {$lte: 43}};
                const pred = makeIndexCompatPred(keyPattern, wildFieldPred);

                let blockingSort = true;
                // A sort only on the regular prefix field can get a nonblocking sort.
                if (sort.hasOwnProperty('pre') && Object.keys(sort).length === 1) {
                    blockingSort = false;
                }

                runSortTest({pred, sort, proj, blockingSort: blockingSort});
            }
        }

        coll.dropIndexes();
    }
}

initializeDocs();
testIndexesForWildcardField("wild.$**", ["wild.num1", "wild.str1"]);
testIndexesForWildcardField("$**", ["num2", "str2"]);
})();
