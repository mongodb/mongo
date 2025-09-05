/**
 * Verify that the rawData argument is propagated through for a findAndModify command on a sharded time-series collection without a shardKey.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const numShards = 2;
const st = new ShardingTest({shards: numShards});

const dbName = "testDB";
const collName = "testColl";
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);
const timeField = "time";
const metaField = "meta";

function setUpShardedCluster() {
    assert.commandWorked(
        mongosDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
    );
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    let shardKey = {[metaField]: 1};

    assert.commandWorked(mongosColl.createIndex(shardKey));
    mongosDB.adminCommand({
        shardCollection: mongosColl.getFullName(),
        key: shardKey,
    });

    const numberDoc = 100;
    for (let i = 0; i < numberDoc; i++) {
        assert.commandWorked(mongosColl.insert({[timeField]: ISODate("2025-09-03T10:00:00Z"), [metaField]: i}));
    }

    let splitPoint = {[metaField]: 50};

    assert.commandWorked(
        mongosDB.adminCommand({
            split: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(),
            middle: splitPoint,
        }),
    );

    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(),
            find: {[metaField]: 25},
            to: st.shard0.shardName,
            _waitForDelete: true,
        }),
    );

    assert.commandWorked(
        mongosDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(),
            find: splitPoint,
            to: st.shard1.shardName,
            _waitForDelete: true,
        }),
    );
}

setUpShardedCluster();

function runFindAndModifyExplainTest(isRawData) {
    const rawDataSpec = isRawData ? getRawOperationSpec(mongosDB) : {};
    const coll = isRawData ? getTimeseriesCollForRawOps(mongosDB, mongosColl) : mongosColl;

    const res = assert.commandWorked(
        coll.runCommand({
            explain: {
                findAndModify: coll.getName(),
                query: {[metaField]: {$gte: 40, $lte: 60}},
                update: {$set: {[metaField]: 80}},
                ...rawDataSpec,
            },
        }),
    );

    const expectedWinningPlan = isRawData ? "UPDATE" : "TS_MODIFY";
    assert.eq(getWinningPlanFromExplain(res).stage, expectedWinningPlan, res);
}

runFindAndModifyExplainTest(true);
runFindAndModifyExplainTest(false);

st.stop();
