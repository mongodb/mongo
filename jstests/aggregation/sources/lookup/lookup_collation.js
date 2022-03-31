/**
 * Tests that $lookup respects the user-specified collation or the inherited local collation
 * when performing comparisons on a foreign collection with a different default collation. Exercises
 * the fix for SERVER-43350.
 *
 * Collation can be set at three different levels for $lookup stage
 *  1. on the local collection (collation on the foreign collection is always ignored)
 *  2. on the $lookup stage via '_internalCollation' property
 *  3. on the aggregation command via 'collation' property in options
 *
 * The three settings have the following precedence:
 *  1. '_internalCollation' overrides all others
 *  2. 'collation' option overrides local collection's collation
 */
load("jstests/aggregation/extras/utils.js");  // For anyEq.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

(function() {

"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const caseInsensitive = {
    locale: "en_US",
    strength: 1
};

const caseSensitive = {
    locale: "simple"
};

// When no collation is specified for a collection, it uses the default, case-sensitive collation.
assert.commandWorked(testDB.createCollection("case_sensitive"));
const collAa = testDB.case_sensitive;
assert.commandWorked(testDB.createCollection("case_sensitive_indexed"));
const collAa_indexed = testDB.case_sensitive_indexed;

assert.commandWorked(testDB.createCollection("case_insensitive", {collation: caseInsensitive}));
const collAA = testDB.case_insensitive;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(collAA) && !isShardedLookupEnabled) {
    return;
}

const records = [{_id: 0, key: "a"}, {_id: 1, key: "A"}];
assert.commandWorked(collAa.insert(records));
assert.commandWorked(collAA.insert(records));
assert.commandWorked(collAa_indexed.insert(records));
assert.commandWorked(collAa_indexed.createIndex({key: 1}));

const lookupWithPipeline = (foreignColl) => {
    return {
        $lookup: {
            from: foreignColl.getName(),
            as: "matched",
            let: {l_key: "$key"},
            pipeline: [{$match: {$expr: {$eq: ["$key", "$$l_key"]}}}]
        }
    };
};
const lookupNoPipeline = (foreignColl) => {
    return {
        $lookup:
            {from: foreignColl.getName(), localField: "key", foreignField: "key", as: "matched"}
    };
};

const resultCaseSensistive = [
    {_id: 0, key: "a", matched: [{_id: 0, key: "a"}]},
    {_id: 1, key: "A", matched: [{_id: 1, key: "A"}]},
];
const resultCaseInsensitive = [
    {_id: 0, key: "a", matched: [{_id: 0, key: "a"}, {_id: 1, key: "A"}]},
    {_id: 1, key: "A", matched: [{_id: 0, key: "a"}, {_id: 1, key: "A"}]},
];
let results = [];

// Collation on the foreign collection should be ignored.
(function testLocalCollationPrecedence() {
    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        results = collAa.aggregate([lookupInto(collAA)]).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensistive,
            extraErrorMsg: " Default collation on local, running: " + tojson(lookupInto)
        });

        results = collAA.aggregate([lookupInto(collAa)]).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, running: " + tojson(lookupInto)
        });

        // When lowering to SBE a different join algorithm (HashJoin) is used if 'allowDiskUse' is
        // set to true. We only need to verify the collation of HJ once, because it works the same
        // independent of how the collation is chosen.
        results = collAA.aggregate([lookupInto(collAa)], {allowDiskUse: true}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, disk use allowed, running: " +
                tojson(lookupInto)
        });
    }
})();

// Collation at the command level should override collation of the local collection.
(function testCommandCollationPrecedence() {
    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        results = collAa.aggregate([lookupInto(collAa)], {collation: caseInsensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on command, running: " + tojson(lookupInto)
        });

        results = collAA.aggregate([lookupInto(collAa)], {collation: caseSensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensistive,
            extraErrorMsg: " Case-sensitive collation on command, running: " + tojson(lookupInto)
        });
    }
})();

// Collation set on $lookup stage with '_internalCollation' should override collation of the local
// collection and on the command.
(function testStageCollationPrecedence() {
    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        let lookupStage = lookupInto(collAa);
        lookupStage.$lookup._internalCollation = caseInsensitive;
        results = collAa.aggregate([lookupStage], {collation: caseSensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on stage, running: " + tojson(lookupInto)
        });

        lookupStage.$lookup._internalCollation = caseSensitive;
        results = collAA.aggregate([lookupStage], {collation: caseInsensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensistive,
            extraErrorMsg: " Case-sensitive collation on stage, running: " + tojson(lookupInto)
        });
    }
})();

// In presense of indexes lookup might choose a different strategy for the join, that relies on the
// index (INLJ). It should respect the effective collation of $lookup.
(function testCollationWithIndexes() {
    // TODO SERVER-65115: integration of collation with INLJ NYI.
    if (checkSBEEnabled(testDB, ["featureFlagSBELookupPushdown"])) {
        jsTestLog("Skipping test because of SERVER-65115.");
        return;
    }

    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        // Local and foreign have different collations.
        results = collAA.aggregate([lookupInto(collAa_indexed)]).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, foreign is indexed, running: " +
                tojson(lookupInto)
        });

        // Command-level collation overrides collection-level collation.
        results =
            collAa.aggregate([lookupInto(collAa_indexed)], {collation: caseInsensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on command, foreign is indexed, running: " +
                tojson(lookupInto)
        });

        // Stage-level collation overrides collection-level and command-level collations.
        let lookupStage = lookupInto(collAa_indexed);
        lookupStage.$lookup._internalCollation = caseInsensitive;
        results = collAa.aggregate([lookupStage], {collation: caseSensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on stage, foreign is indexed, running: " +
                tojson(lookupInto)
        });
    }
})();
})();
