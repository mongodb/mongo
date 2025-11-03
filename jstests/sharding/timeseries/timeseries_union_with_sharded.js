/**
 * Tests the behavior of $unionWith and timeseries in a sharded environment. These include tests
 * where the foreign collection is sharded and untracked, and when the local is sharded and untracked.
 * Also runs each test on a view on the foreign collection.
 * @tags: [
 *    requires_sharding,
 *    requires_timeseries
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {aggPlanHasStage, getUnionWithStage} from "jstests/libs/query/analyze_plan.js";

const st = new ShardingTest({mongos: 1, shards: 4, config: 1});

const testDBName = "test";
const testDB = st.getDB(testDBName);
const metaField = "m";
const timeField = "t";
const local = testDB.local;
const foreignTS = testDB.foreignTimeseries;
const viewOnTs = testDB.viewOnTs;

assert(st.adminCommand({enableSharding: testDBName, primaryShard: st.shard0.shardName}));

function runAndValidateExplain(pipeline) {
    const explain = local.explain().aggregate(pipeline);
    const unionWithStage = getUnionWithStage(explain);
    const hasUnpackStage = aggPlanHasStage(unionWithStage["$unionWith"]["pipeline"], "$_internalUnpackBucket");
    assert(
        hasUnpackStage,
        "Expected to find $_internalUnpackBucket stage in $unionWith subpipeline explain output" + tojson(explain),
    );
}

function insertDocuments() {
    assert.commandWorked(
        local.insertMany([
            {_id: 1, shard_key: "shard1"},
            {_id: 2, shard_key: "shard1"},
            {_id: 3, shard_key: "shard1"},
        ]),
    );

    assert.commandWorked(
        foreignTS.insertMany([
            {_id: 4, [timeField]: ISODate("1999-09-30T04:11:10Z"), [metaField]: "shard2", a: 1},
            {_id: 5, [timeField]: ISODate("1999-09-30T04:13:10Z"), [metaField]: "shard2", a: 2},
            {_id: 6, [timeField]: ISODate("1999-09-30T07:14:10Z"), [metaField]: "shard2", a: 3},
        ]),
    );
}

function runUnionWithAndAssertResults({foreignCollName, isView, extraDocs = []}) {
    const expected = [
        {_id: 1, shard_key: "shard1"},
        {_id: 2, shard_key: "shard1"},
        {_id: 3, shard_key: "shard1"},
        {_id: 5, [timeField]: ISODate("1999-09-30T04:13:10Z"), [metaField]: "shard2", a: 2},
        {_id: 6, [timeField]: ISODate("1999-09-30T07:14:10Z"), [metaField]: "shard2", a: 3},
    ];

    if (!isView) {
        // The view filters out this document.
        expected.push({_id: 4, [timeField]: ISODate("1999-09-30T04:11:10Z"), [metaField]: "shard2", a: 1});
    }
    if (extraDocs.length > 0) {
        expected.push(...extraDocs);
    }
    const results = local.aggregate([{$unionWith: foreignCollName}]).toArray();
    assertArrayEq({actual: results, expected: expected});
}

(function localShardedForeignShardedView() {
    // The local collection is sharded and the data will be on shard1.
    // The foreign collection is sharded and the data will be on shard2.
    // unionWith will run on shard1 and throw 'CommandOnShardedViewNotSupportedOnMongod' exception.
    local.drop();
    assert.commandWorked(local.createIndex({shard_key: 1}));

    foreignTS.drop();
    testDB.createCollection(foreignTS.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    assert.commandWorked(foreignTS.createIndex({[metaField]: 1}));
    insertDocuments();
    assert(st.s.adminCommand({shardCollection: local.getFullName(), key: {shard_key: 1}}));
    assert(st.s.adminCommand({shardCollection: foreignTS.getFullName(), key: {[metaField]: 1}}));

    // Place the local collection on shard1 and all of foreign on shard2 to force
    // 'CommandOnShardedViewNotSupportedOnMongod' exceptions.
    assert.commandWorked(
        testDB.adminCommand({moveChunk: local.getFullName(), find: {shard_key: "shard1"}, to: st.shard1.shardName}),
    );
    assert.commandWorked(
        testDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(testDB, foreignTS).getFullName(),
            find: {meta: "shard2"},
            to: st.shard2.shardName,
        }),
    );
    assert.commandWorked(testDB.createView("unionView", foreignTS.getName(), [{$match: {a: {$gte: 2}}}]));

    runUnionWithAndAssertResults({foreignCollName: "unionView", isView: true});
    runAndValidateExplain([{$unionWith: foreignTS.getName()}]);
})();

(function localTrackedForeignShardedAcrossShards() {
    // The local collection is unsharded but tracked on shard1.
    // The foreign collection is sharded with data on shard2 and shard3.
    // unionWith will run on mongos and shard1 which both have information about the foreign collection.
    local.drop();
    assert.commandWorked(
        testDB.runCommand({createUnsplittableCollection: local.getName(), dataShard: st.shard1.shardName}),
    );
    foreignTS.drop();
    testDB.createCollection(foreignTS.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    insertDocuments();
    // Have an extra doc to be on shard3.
    const extraDoc = {_id: 7, [timeField]: ISODate("1999-09-30T07:14:10Z"), [metaField]: "shard3", a: 3};
    assert.commandWorked(foreignTS.insert(extraDoc));

    assert.commandWorked(foreignTS.createIndex({[metaField]: 1}));
    assert(st.s.adminCommand({shardCollection: foreignTS.getFullName(), key: {[metaField]: 1}}));
    assert.commandWorked(
        testDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(testDB, foreignTS).getFullName(),
            find: {meta: "shard2"},
            to: st.shard2.shardName,
        }),
    );
    assert.commandWorked(
        testDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(testDB, foreignTS).getFullName(),
            find: {meta: "shard3"},
            to: st.shard3.shardName,
        }),
    );

    runUnionWithAndAssertResults({foreignCollName: foreignTS.getName(), isView: false, extraDocs: [extraDoc]});
    runAndValidateExplain([{$unionWith: foreignTS.getName()}]);

    // Run the same test but on a view on the foreign collection.
    assert.commandWorked(testDB.createView(viewOnTs.getName(), foreignTS.getName(), [{$match: {a: {$gte: 2}}}]));
    runUnionWithAndAssertResults({foreignCollName: viewOnTs.getName(), isView: true, extraDocs: [extraDoc]});
})();

(function localShardedForeignUntrackedDiffShards() {
    // The local collection is sharded and the data will be on shard1.
    // The foreign collection will be untracked and be on the primary shard (shard0).
    // unionWith will run on mongos and shard1, which both do not have any information about the foreign collection.
    local.drop();
    foreignTS.drop();
    testDB.createCollection(foreignTS.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    assert.commandWorked(testDB.adminCommand({untrackUnshardedCollection: foreignTS.getFullName()}));

    insertDocuments();
    assert.commandWorked(local.createIndex({shard_key: 1}));
    assert(st.s.adminCommand({shardCollection: local.getFullName(), key: {shard_key: 1}}));
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: local.getFullName(),
            find: {shard_key: "shard1"},
            to: st.shard1.shardName,
        }),
    );

    runUnionWithAndAssertResults({foreignCollName: foreignTS.getName(), isView: false});
    runAndValidateExplain([{$unionWith: foreignTS.getName()}]);

    // Run the same test but on a view on the foreign collection.
    assert.commandWorked(testDB.createView(viewOnTs.getName(), foreignTS.getName(), [{$match: {a: {$gte: 2}}}]));
    runUnionWithAndAssertResults({foreignCollName: viewOnTs.getName(), isView: true});
})();

(function localUntrackedForeignTrackedDiffShards() {
    // The local collection is untracked and lives on the primary shard (shard0).
    // The foreign is tracked, unsharded, and lives on shard2.
    // unionWith will run on mongos and shard1 which both have information about the foreign collection.
    local.drop();
    foreignTS.drop();
    testDB.createCollection(foreignTS.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    insertDocuments();
    assert.commandWorked(testDB.adminCommand({untrackUnshardedCollection: local.getFullName()}));
    assert.commandWorked(
        testDB.adminCommand({
            moveCollection: getTimeseriesCollForDDLOps(testDB, foreignTS).getFullName(),
            toShard: st.shard1.shardName,
        }),
    );

    runUnionWithAndAssertResults({foreignCollName: foreignTS.getName(), isView: false});
    runAndValidateExplain([{$unionWith: foreignTS.getName()}]);

    // same test but on a view for foreign.
    assert.commandWorked(testDB.createView(viewOnTs.getName(), foreignTS.getName(), [{$match: {a: {$gte: 2}}}]));
    runUnionWithAndAssertResults({foreignCollName: viewOnTs.getName(), isView: true});
})();

st.stop();
