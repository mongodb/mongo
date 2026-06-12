/**
 * Tests `fixedBucketing` behavior on sharded timeseries collections:
 *  - `shardCollection` persists the value in `config.collections`.
 *  - Mismatched values across `createCollection` / `shardCollection` are rejected.
 *  - `collMod` requests that modify bucketing parameters automatically set `fixedBucketing` to
 *    `false` in both the global catalog (`config.collections`) and the durable catalog (visible
 *    via `listCollections`).
 *
 * Only runs when both `featureFlagFixedBucketingCatalog` and
 * `featureFlagCreateViewlessTimeseriesCollections` are enabled.
 *
 * @tags: [
 *   featureFlagFixedBucketingCatalog,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   requires_fcv_90,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const dbName = "test";
const collName = "ts";
const collNss = `${dbName}.${collName}`;
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
const sDB = st.s.getDB(dbName);

const fbTimeseries = {timeField: "time", metaField: "hostId", fixedBucketing: true};

// Reads the `fixedBucketing` value persisted for the test collection in `config.collections`.
function getConfigFixedBucketing() {
    return st.s
        .getDB("config")
        .collections.findOne({_id: `${dbName}.${getTimeseriesCollForDDLOps(sDB, collName)}`})
        .timeseriesFields.fixedBucketing;
}

// Reads the `fixedBucketing` value visible via `listCollections` for the test collection.
function getListCollFixedBucketing() {
    const colls = sDB.runCommand({listCollections: 1, filter: {type: "timeseries", name: collName}})
        .cursor.firstBatch;
    assert.eq(colls.length, 1, colls);
    return colls[0].options.timeseries.fixedBucketing;
}

// shardCollection creates the collection with fixedBucketing: true. Both config.collections and
// listCollections must reflect the value.
(function implicitCreateWithFixedBucketing() {
    assert.commandWorked(
        st.s.adminCommand({shardCollection: collNss, key: {"hostId": 1}, timeseries: fbTimeseries}),
    );
    assert.eq(getConfigFixedBucketing(), true);
    assert.eq(getListCollFixedBucketing(), true);
    assert(sDB.ts.drop());
})();

// Existing collection created with fixedBucketing: true, sharded without re-specifying the
// timeseries options. The field must be preserved in both catalogs.
(function existingCollectionInheritsFixedBucketing() {
    assert.commandWorked(sDB.createCollection(collName, {timeseries: fbTimeseries}));
    assert.commandWorked(st.s.adminCommand({shardCollection: collNss, key: {"hostId": 1}}));
    assert.eq(getConfigFixedBucketing(), true);
    assert.eq(getListCollFixedBucketing(), true);
    assert(sDB.ts.drop());
})();

// collMod that changes bucketing parameters on a sharded fixedBucketing: true collection must
// set fixedBucketing to false in both the global catalog (`config.collections`) and the durable
// catalog (visible via `listCollections`).
(function collModBucketingChangeDisablesFixedBucketing() {
    assert.commandWorked(
        st.s.adminCommand({shardCollection: collNss, key: {"hostId": 1}, timeseries: fbTimeseries}),
    );
    assert.eq(getConfigFixedBucketing(), true);
    assert.eq(getListCollFixedBucketing(), true);

    assert.commandWorked(sDB.runCommand({collMod: collName, timeseries: {granularity: "hours"}}));
    assert.eq(getConfigFixedBucketing(), false);
    assert.eq(getListCollFixedBucketing(), false);

    assert(sDB.ts.drop());
})();

// The six cases below cover every cross-pair of values where the value used at createCollection
// differs from the one passed to shardCollection, under optionsAreEqual's strict semantics
// (unset, false, true are three distinct states). Each must fail with InvalidOptions.
function tsWithFixedBucketing(fixedBucketing) {
    const ts = {timeField: "time", metaField: "hostId"};
    if (fixedBucketing !== undefined) {
        ts.fixedBucketing = fixedBucketing;
    }
    return ts;
}

const mismatchedPairs = [
    {createVal: true, shardVal: false},
    {createVal: true, shardVal: undefined},
    {createVal: false, shardVal: true},
    {createVal: false, shardVal: undefined},
    {createVal: undefined, shardVal: true},
    {createVal: undefined, shardVal: false},
];

for (const {createVal, shardVal} of mismatchedPairs) {
    (function () {
        assert.commandWorked(
            sDB.createCollection(collName, {timeseries: tsWithFixedBucketing(createVal)}),
        );
        assert.commandFailedWithCode(
            st.s.adminCommand({
                shardCollection: collNss,
                key: {"hostId": 1},
                timeseries: tsWithFixedBucketing(shardVal),
            }),
            [ErrorCodes.InvalidOptions],
        );
        assert(sDB.ts.drop());
    })();
}

st.stop();
