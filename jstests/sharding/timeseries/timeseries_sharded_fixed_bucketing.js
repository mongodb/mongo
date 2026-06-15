/**
 * Tests `fixedBucketing` behavior on sharded timeseries collections:
 *  - `shardCollection` persists the value in `config.collections`.
 *  - New viewless timeseries collections default `fixedBucketing` to `true` when the field is
 *    omitted from `createCollection` or `shardCollection`.
 *  - Mismatched values across `createCollection` / `shardCollection` are rejected.
 *  - `collMod` requests that modify bucketing parameters automatically set `fixedBucketing` to
 *    `false` in both the global catalog (`config.collections`) and the durable catalog (visible
 *    via `listCollections`).
 *  - Re-creates and re-shards: omitting `fixedBucketing` inherits the stored value (succeeds),
 *    an explicit matching value succeeds, and an explicit mismatching value is rejected.
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

// Builds timeseries options with the given fixedBucketing value; an undefined value omits the field.
function tsWithFixedBucketing(fixedBucketing) {
    const ts = {timeField: "time", metaField: "hostId"};
    if (fixedBucketing !== undefined) {
        ts.fixedBucketing = fixedBucketing;
    }
    return ts;
}

// shardCollection without a `timeseries` argument shards an existing timeseries collection without
// restating its options, preserving the stored fixedBucketing value regardless of the value the
// collection was created with (an omitted create value defaults to true).
(function shardExistingCollectionWithoutTimeseriesArg() {
    for (const createVal of [undefined, true, false]) {
        assert.commandWorked(
            sDB.createCollection(collName, {timeseries: tsWithFixedBucketing(createVal)}),
        );
        assert.commandWorked(st.s.adminCommand({shardCollection: collNss, key: {"hostId": 1}}));
        const stored = createVal === undefined ? true : createVal;
        assert.eq(getConfigFixedBucketing(), stored, {createVal});
        assert.eq(getListCollFixedBucketing(), stored, {createVal});
        assert(sDB.ts.drop());
    }
})();

// Exhaustively cover sharding a new collection followed by re-create and re-shard.
// shardVal is the value passed to the initial shardCollection; an omitted value defaults to true.
// recreateVal is the value passed to re-create and re-shard; omitting it inherits the stored value
// (succeeds), matching it succeeds, and mismatching it fails.
// Parameters: (shardVal, recreateVal, valid).
(function shardRecreateReshardCases() {
    const cases = [
        // shardVal=undefined (stored=true): omit inherits, true matches, false mismatches
        {shardVal: undefined, recreateVal: undefined, valid: true},
        {shardVal: undefined, recreateVal: true, valid: true},
        {shardVal: undefined, recreateVal: false, valid: false},
        // shardVal=true (stored=true): same behavior as omit, but exercises explicit-true path
        {shardVal: true, recreateVal: undefined, valid: true},
        {shardVal: true, recreateVal: true, valid: true},
        {shardVal: true, recreateVal: false, valid: false},
        // shardVal=false (stored=false): omit inherits, false matches, true mismatches
        {shardVal: false, recreateVal: undefined, valid: true},
        {shardVal: false, recreateVal: false, valid: true},
        {shardVal: false, recreateVal: true, valid: false},
    ];

    for (const tc of cases) {
        const stored = tc.shardVal === undefined ? true : tc.shardVal;

        // Shard the collection (implicitly creates it) with the initial value.
        assert.commandWorked(
            st.s.adminCommand({
                shardCollection: collNss,
                key: {"hostId": 1},
                timeseries: tsWithFixedBucketing(tc.shardVal),
            }),
        );

        // Re-create with recreateVal.
        const recreateRes = sDB.createCollection(collName, {
            timeseries: tsWithFixedBucketing(tc.recreateVal),
        });
        if (tc.valid) {
            assert.commandWorked(recreateRes, tc);
            assert.eq(getConfigFixedBucketing(), stored, tc);
            assert.eq(getListCollFixedBucketing(), stored, tc);

            // Re-shard with recreateVal also succeeds.
            assert.commandWorked(
                st.s.adminCommand({
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
            // Re-shard with the mismatching value also fails.
            assert.commandFailedWithCode(
                st.s.adminCommand({
                    shardCollection: collNss,
                    key: {"hostId": 1},
                    timeseries: tsWithFixedBucketing(tc.recreateVal),
                }),
                [ErrorCodes.InvalidOptions],
                tc,
            );
        }

        assert(sDB.ts.drop());
    }
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

// Exhaustively cover createCollection(createVal) followed by shardCollection(shardVal). An omitted
// value on createCollection defaults to true; an omitted value on shardCollection inherits the
// stored value. The shard is valid when the resolved values match, else it is rejected with
// InvalidOptions.
(function createShardCases() {
    const cases = [
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
    for (const tc of cases) {
        assert.commandWorked(
            sDB.createCollection(collName, {timeseries: tsWithFixedBucketing(tc.createVal)}),
        );
        const shardRes = st.s.adminCommand({
            shardCollection: collNss,
            key: {"hostId": 1},
            timeseries: tsWithFixedBucketing(tc.shardVal),
        });
        if (tc.valid) {
            // createCollection stores createVal (an omitted value defaults to true); an omitted
            // shardVal inherits it, so the stored value is always the create-time one.
            const stored = tc.createVal === undefined ? true : tc.createVal;
            assert.commandWorked(shardRes, tc);
            assert.eq(getConfigFixedBucketing(), stored, tc);
            assert.eq(getListCollFixedBucketing(), stored, tc);
        } else {
            assert.commandFailedWithCode(shardRes, [ErrorCodes.InvalidOptions], tc);
        }
        assert(sDB.ts.drop());
    }
})();

st.stop();
