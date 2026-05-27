/**
 * Tests the `fixedBucketing` timeseries option on `createCollection`: that the value is correctly
 * persisted in the catalog, that idempotent re-creates succeed, and that conflicting re-creates
 * fail with `NamespaceExists`.
 *
 * Runs only when `featureFlagFixedBucketingCatalog` is enabled. Because the field is only valid on
 * viewless timeseries collections, the test also requires
 * `featureFlagCreateViewlessTimeseriesCollections`.
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
// omits the field entirely (distinct from passing `false`). Returns the raw command result so the
// caller can decide between `assert.commandWorked` and `assert.commandFailedWithCode`.
function createWithFixedBucketing(fixedBucketing) {
    const timeseries = {timeField: "t"};
    if (fixedBucketing !== undefined) {
        timeseries.fixedBucketing = fixedBucketing;
    }
    return testDB.createCollection(collName, {timeseries});
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

    it("omits the field from listCollections when not specified", function () {
        assert.commandWorked(createWithFixedBucketing(undefined));
        assert.eq(getStoredFixedBucketing(), undefined);
    });
});

describe("idempotent create with same fixedBucketing", function () {
    afterEach(function () {
        coll.drop();
    });

    it("succeeds when both calls set true", function () {
        assert.commandWorked(createWithFixedBucketing(true));
        assert.commandWorked(createWithFixedBucketing(true));
    });

    it("succeeds when both calls set false", function () {
        assert.commandWorked(createWithFixedBucketing(false));
        assert.commandWorked(createWithFixedBucketing(false));
    });

    it("succeeds when both calls omit the field", function () {
        assert.commandWorked(createWithFixedBucketing(undefined));
        assert.commandWorked(createWithFixedBucketing(undefined));
    });
});

describe("conflicting create with mismatched fixedBucketing", function () {
    afterEach(function () {
        coll.drop();
    });

    // Unset, false, and true are three distinct states. Any combination that differs across the two create calls must
    // fail with NamespaceExists when matchesStorageOptions detects the timeseries options have changed.
    const mismatchedPairs = [
        {existing: true, retry: false},
        {existing: true, retry: undefined},
        {existing: false, retry: true},
        {existing: false, retry: undefined},
        {existing: undefined, retry: true},
        {existing: undefined, retry: false},
    ];

    for (const {existing, retry} of mismatchedPairs) {
        it(`fails when existing=${existing} and retry=${retry}`, function () {
            assert.commandWorked(createWithFixedBucketing(existing));
            assert.commandFailedWithCode(createWithFixedBucketing(retry), ErrorCodes.NamespaceExists);
        });
    }
});
