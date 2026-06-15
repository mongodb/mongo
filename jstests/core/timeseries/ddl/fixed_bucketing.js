/**
 * Tests the `fixedBucketing` timeseries option:
 *  - Persistence: explicit true/false is stored correctly; omitting the field defaults to true.
 *  - Re-create behavior: omitting the field on re-create inherits the stored value; an explicit
 *    matching value succeeds; an explicit mismatching value fails with `NamespaceExists`.
 *  - collMod: changing bucketing parameters sets `fixedBucketing` to false.
 *
 * Runs only when `featureFlagFixedBucketingCatalog` and
 * `featureFlagCreateViewlessTimeseriesCollections` are enabled.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagFixedBucketingCatalog,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   # featureFlagFixedBucketingCatalog is fcv_gated; this tag prevents the test from running
 *   # in FCV-downgrade passthroughs where the feature would be disabled mid-suite.
 *   requires_fcv_90,
 * ]
 */
import {afterEach, describe, it} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const collName = "ts";
const coll = testDB[collName];

// Issues `createCollection` for the test's timeseries collection. The `fixedBucketing` field is
// included in the request only when `fixedBucketing` is `true` or `false`; passing `undefined`
// omits the field entirely (distinct from passing `false`). The optional second argument inserts
// `bucketMaxSpanSeconds` and `bucketRoundingSeconds` when provided. Returns the raw command result
// so the caller can decide between `assert.commandWorked` and `assert.commandFailedWithCode`.
function createWithFixedBucketing(fixedBucketing, {maxSpanSeconds, roundingSeconds} = {}) {
    const timeseries = {timeField: "t"};
    if (maxSpanSeconds !== undefined) timeseries.bucketMaxSpanSeconds = maxSpanSeconds;
    if (roundingSeconds !== undefined) timeseries.bucketRoundingSeconds = roundingSeconds;
    if (fixedBucketing !== undefined) timeseries.fixedBucketing = fixedBucketing;
    return testDB.createCollection(collName, {timeseries});
}

// Issues a `collMod` that sets `bucketMaxSpanSeconds` and `bucketRoundingSeconds`. Returns the raw
// command result so the caller can decide how to assert.
function collModBucketing({maxSpanSeconds, roundingSeconds}) {
    return testDB.runCommand({
        collMod: collName,
        timeseries: {bucketMaxSpanSeconds: maxSpanSeconds, bucketRoundingSeconds: roundingSeconds},
    });
}

// Reads the test's timeseries collection back via `listCollections` and returns its stored
// `fixedBucketing` value. Returns `undefined` when the field is not present in the catalog, since
// `OptionalBool` omits unset fields from BSON.
function getStoredFixedBucketing() {
    const colls = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

describe("fixedBucketing persistence", function () {
    afterEach(function () {
        coll.drop();
    });

    it("stores true and returns it via listCollections", function () {
        assert.commandWorked(createWithFixedBucketing(true));
        assert.eq(getStoredFixedBucketing(), true);
    });

    it("stores false and returns it via listCollections", function () {
        assert.commandWorked(createWithFixedBucketing(false));
        assert.eq(getStoredFixedBucketing(), false);
    });

    it("defaults the stored value to true when the field is omitted on create", function () {
        assert.commandWorked(createWithFixedBucketing(undefined));
        assert.eq(getStoredFixedBucketing(), true);
    });
});

// Exhaustively cover createCollection(createVal) followed by re-create(recreateVal).
// An omitted createVal defaults to true. An omitted recreateVal inherits the stored value
// (succeeds); an explicit matching value also succeeds; an explicit mismatching value fails.
describe("re-create behavior", function () {
    afterEach(function () {
        coll.drop();
    });

    const cases = [
        // Omit on re-create: inherits the stored value.
        {createVal: true, recreateVal: undefined, valid: true},
        {createVal: false, recreateVal: undefined, valid: true},
        {createVal: undefined, recreateVal: undefined, valid: true}, // stored=true
        // Same explicit value: matches stored value.
        {createVal: true, recreateVal: true, valid: true},
        {createVal: false, recreateVal: false, valid: true},
        // Omit on first create (stored=true); explicit true on re-create matches.
        {createVal: undefined, recreateVal: true, valid: true},
        // Mismatched explicit value: fails with NamespaceExists.
        {createVal: true, recreateVal: false, valid: false},
        {createVal: false, recreateVal: true, valid: false},
        // Omit on first create (stored=true); explicit false conflicts.
        {createVal: undefined, recreateVal: false, valid: false},
    ];

    for (const {createVal, recreateVal, valid} of cases) {
        it(`create(${createVal}) then re-create(${recreateVal}) → ${valid ? "succeeds" : "NamespaceExists"}`, function () {
            const stored = createVal === undefined ? true : createVal;
            assert.commandWorked(createWithFixedBucketing(createVal));
            if (valid) {
                assert.commandWorked(createWithFixedBucketing(recreateVal));
                assert.eq(getStoredFixedBucketing(), stored);
            } else {
                assert.commandFailedWithCode(
                    createWithFixedBucketing(recreateVal),
                    ErrorCodes.NamespaceExists,
                );
            }
        });
    }
});

describe("collMod fixedBucketing handling for bucketing changes", function () {
    afterEach(function () {
        coll.drop();
    });

    const cases = [
        {initial: true, newSpan: 200, expected: false, desc: "true → false on bucketing change"},
        {
            initial: false,
            newSpan: 200,
            expected: false,
            desc: "false stays false on bucketing change",
        },
        // Omitting fixedBucketing on create stores true; a bucketing change then flips it to false.
        {
            initial: undefined,
            newSpan: 200,
            expected: false,
            desc: "omitted (stored true) → false on bucketing change",
        },
        {
            initial: true,
            newSpan: 100,
            expected: true,
            desc: "true stays true when bucketing unchanged",
        },
        {
            initial: false,
            newSpan: 100,
            expected: false,
            desc: "false stays false when bucketing unchanged",
        },
        // Omitting fixedBucketing on create stores true; no bucketing change leaves it as true.
        {
            initial: undefined,
            newSpan: 100,
            expected: true,
            desc: "omitted (stored true) stays true when bucketing unchanged",
        },
    ];

    for (const {initial, newSpan, expected, desc} of cases) {
        it(desc, function () {
            assert.commandWorked(
                createWithFixedBucketing(initial, {maxSpanSeconds: 100, roundingSeconds: 100}),
            );
            assert.commandWorked(
                collModBucketing({maxSpanSeconds: newSpan, roundingSeconds: newSpan}),
            );
            assert.eq(getStoredFixedBucketing(), expected);
        });
    }

    // Granularity changes go through the same code path as explicit span/rounding changes.
    // Only test the significant true → false case; the other cases (where fixedBucketing stays
    // unchanged) are already covered by the parameterized loop above.
    it("true → false on granularity change", function () {
        assert.commandWorked(createWithFixedBucketing(true));
        assert.commandWorked(
            testDB.runCommand({collMod: collName, timeseries: {granularity: "minutes"}}),
        );
        assert.eq(getStoredFixedBucketing(), false);
    });
});
