/**
 * Tests `fixedBucketing` behavior on sharded timeseries collections:
 *  - `shardCollection` persists the value in `config.collections`.
 *  - New viewless timeseries collections default `fixedBucketing` to `true` when the field is omitted from
 *    `createCollection` or `shardCollection`.
 *  - Mismatched values across `createCollection`/`shardCollection` are rejected.
 *  - `collMod` requests that modify bucketing parameters set `fixedBucketing` to `false` in both the global catalog
 *    (`config.collections`) and the durable catalog (visible via `listCollections`).
 *  - Re-creates and re-shards: omitting `fixedBucketing` inherits the stored value (succeeds), an explicit matching
 *    value succeeds, and an explicit mismatching value is rejected.
 *
 * @tags: [
 *   featureFlagFixedBucketingCatalog,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   # Avoid suites that can trigger transitions to FCV < 9.0, which would make the observed fixedBucketing value
 *   # unstable (because it is stripped on downgrades and re-added and set to false on upgrades).
 *   requires_fcv_90,
 *   # Avoid auto-sharding: this test performs explicit calls to shardCollection
 *   assumes_unsharded_collection,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {findTimeseriesConfigCollectionsDocument} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const mongos = db.getMongo();
const testDB = db.getSiblingDB(jsTestName());
const collName = "ts";
const collNss = `${testDB.getName()}.${collName}`;

assert.commandWorked(testDB.dropDatabase());

// Builds timeseries options with the optional fixedBucketing field; undefined omits the field.
function tsWithFixedBucketing(fixedBucketing) {
    const ts = {timeField: "time", metaField: "hostId"};
    if (fixedBucketing !== undefined) {
        ts.fixedBucketing = fixedBucketing;
    }
    return ts;
}

// Reads fixedBucketing for the test collection from the global catalog (config.collections).
function getConfigFixedBucketing() {
    const entry = findTimeseriesConfigCollectionsDocument(testDB[collName]);
    assert(entry, "expected config.collections entry for sharded collection", {collNss});
    return entry.timeseriesFields.fixedBucketing;
}

// Reads fixedBucketing for the test collection via listCollections.
function getListCollFixedBucketing() {
    const colls = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}}),
    ).cursor.firstBatch;
    assert.eq(colls.length, 1, "expected exactly one timeseries collection", {colls});
    return colls[0].options.timeseries.fixedBucketing;
}

// ── shardCollection behavior ──────────────────────────────────────────────────────────────────

describe("shardCollection behavior with fixedBucketing", function () {
    beforeEach(function () {
        assert.commandWorked(mongos.adminCommand({enableSharding: testDB.getName()}));
    });
    afterEach(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    // shardCollection without a timeseries argument shards an existing timeseries collection without restating its
    // options, preserving the stored fixedBucketing value regardless of how the collection was created.
    for (const createVal of [undefined, true, false]) {
        it(`sharding existing collection (created with fixedBucketing=${createVal}) preserves the stored value`, function () {
            assert.commandWorked(
                testDB.createCollection(collName, {timeseries: tsWithFixedBucketing(createVal)}),
            );
            assert.commandWorked(
                mongos.adminCommand({shardCollection: collNss, key: {"hostId": 1}}),
            );
            // Omitting fixedBucketing on creation defaults it to true.
            const stored = createVal === undefined ? true : createVal;
            assert.eq(getConfigFixedBucketing(), stored, {createVal});
            assert.eq(getListCollFixedBucketing(), stored, {createVal});
        });
    }

    // Exhaustively cover shardCollection(fixedBucketing: shardVal) followed by re-create and re-shard.
    // An omitted shardVal defaults to true. An omitted recreateVal/reshardVal inherits the stored value (succeeds); an
    // explicit matching value also succeeds; an explicit mismatching value fails.
    const reshardCases = [
        // shardVal=undefined (stored=true): omit inherits, true matches, false mismatches.
        {shardVal: undefined, recreateVal: undefined, valid: true},
        {shardVal: undefined, recreateVal: true, valid: true},
        {shardVal: undefined, recreateVal: false, valid: false},
        // shardVal=true (stored=true): same behavior as omit, exercises explicit-true path.
        {shardVal: true, recreateVal: undefined, valid: true},
        {shardVal: true, recreateVal: true, valid: true},
        {shardVal: true, recreateVal: false, valid: false},
        // shardVal=false (stored=false): omit inherits, false matches, true mismatches.
        {shardVal: false, recreateVal: undefined, valid: true},
        {shardVal: false, recreateVal: false, valid: true},
        {shardVal: false, recreateVal: true, valid: false},
    ];

    for (const tc of reshardCases) {
        it(`shard(${tc.shardVal}) → recreate(${tc.recreateVal}) → ${tc.valid ? "succeeds" : "fails"}`, function () {
            const stored = tc.shardVal === undefined ? true : tc.shardVal;

            assert.commandWorked(
                mongos.adminCommand({
                    shardCollection: collNss,
                    key: {"hostId": 1},
                    timeseries: tsWithFixedBucketing(tc.shardVal),
                }),
            );

            const recreateRes = testDB.createCollection(collName, {
                timeseries: tsWithFixedBucketing(tc.recreateVal),
            });
            if (tc.valid) {
                assert.commandWorked(recreateRes, tc);
                assert.eq(getConfigFixedBucketing(), stored, tc);
                assert.eq(getListCollFixedBucketing(), stored, tc);

                assert.commandWorked(
                    mongos.adminCommand({
                        shardCollection: collNss,
                        key: {"hostId": 1},
                        timeseries: tsWithFixedBucketing(tc.recreateVal),
                    }),
                    tc,
                );
                assert.eq(getConfigFixedBucketing(), stored, tc);
                assert.eq(getListCollFixedBucketing(), stored, tc);
            } else {
                assert.commandFailedWithCode(recreateRes, [ErrorCodes.NamespaceExists], tc);
                assert.commandFailedWithCode(
                    mongos.adminCommand({
                        shardCollection: collNss,
                        key: {"hostId": 1},
                        timeseries: tsWithFixedBucketing(tc.recreateVal),
                    }),
                    [ErrorCodes.InvalidOptions],
                    tc,
                );
            }
        });
    }
});

// ── Global catalog — createCollection then shardCollection ────────────────────────────────────

// Exhaustively cover createCollection(fixedBucketing: createVal) followed by shardCollection(fixedBucketing: shardVal).
// An omitted value on createCollection defaults to true; an omitted value on shardCollection inherits the stored value.
// The shard operation is valid when the resolved values match, else rejected with InvalidOptions.
describe("global catalog — createCollection then shardCollection", function () {
    beforeEach(function () {
        assert.commandWorked(mongos.adminCommand({enableSharding: testDB.getName()}));
    });
    afterEach(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    const createShardCases = [
        {createVal: true, shardVal: true, valid: true},
        {createVal: true, shardVal: false, valid: false},
        {createVal: true, shardVal: undefined, valid: true},
        {createVal: false, shardVal: true, valid: false},
        {createVal: false, shardVal: false, valid: true},
        {createVal: false, shardVal: undefined, valid: true},
        {createVal: undefined, shardVal: true, valid: true},
        {createVal: undefined, shardVal: false, valid: false},
        {createVal: undefined, shardVal: undefined, valid: true},
    ];

    for (const tc of createShardCases) {
        it(`create(${tc.createVal}) then shard(${tc.shardVal}) → ${tc.valid ? "stored=" + (tc.createVal === undefined ? true : tc.createVal) : "InvalidOptions"}`, function () {
            assert.commandWorked(
                testDB.createCollection(collName, {timeseries: tsWithFixedBucketing(tc.createVal)}),
            );
            const shardRes = mongos.adminCommand({
                shardCollection: collNss,
                key: {"hostId": 1},
                timeseries: tsWithFixedBucketing(tc.shardVal),
            });
            if (tc.valid) {
                // createCollection stores createVal (an omitted value defaults to true); an
                // omitted shardVal inherits it, so the stored value is always the create-time
                // one.
                const stored = tc.createVal === undefined ? true : tc.createVal;
                assert.commandWorked(shardRes, tc);
                assert.eq(getConfigFixedBucketing(), stored, tc);
                assert.eq(getListCollFixedBucketing(), stored, tc);
            } else {
                assert.commandFailedWithCode(shardRes, [ErrorCodes.InvalidOptions], tc);
            }
        });
    }
});

// ── Global catalog — collMod bucketing change on a sharded collection ─────────────────────────

describe("global catalog — collMod bucketing change on a sharded collection", function () {
    beforeEach(function () {
        assert.commandWorked(mongos.adminCommand({enableSharding: testDB.getName()}));
    });
    afterEach(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    it("granularity change flips fixedBucketing to false in config.collections and listCollections", function () {
        assert.commandWorked(
            mongos.adminCommand({
                shardCollection: collNss,
                key: {"hostId": 1},
                timeseries: {timeField: "time", metaField: "hostId", fixedBucketing: true},
            }),
        );
        assert.eq(getConfigFixedBucketing(), true);
        assert.eq(getListCollFixedBucketing(), true);

        assert.commandWorked(
            testDB.runCommand({collMod: collName, timeseries: {granularity: "hours"}}),
        );
        assert.eq(getConfigFixedBucketing(), false);
        assert.eq(getListCollFixedBucketing(), false);
    });
});
