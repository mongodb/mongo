/**
 * Tests that findAndModify with rawData: false does not incorrectly enter raw-data mode on a
 * sharded timeseries collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_82,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const timeField = "time";
const metaField = "meta";

describe("findAndModify rawData: false should not enter raw-data mode", function () {
    let st, mongosDB, mongosColl;

    before(function () {
        st = new ShardingTest({shards: 2});
        mongosDB = st.s.getDB(jsTestName());
        mongosColl = mongosDB.getCollection("testColl");

        assert.commandWorked(mongosDB.createCollection(mongosColl.getName(), {timeseries: {timeField, metaField}}));
        assert.commandWorked(st.s.adminCommand({enableSharding: mongosDB.getName()}));
        assert.commandWorked(mongosColl.createIndex({[metaField]: 1}));
        assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {[metaField]: 1}}));

        // Split and distribute chunks across two shards.
        const bucketsColl = getTimeseriesCollForDDLOps(mongosDB, mongosColl);
        assert.commandWorked(mongosDB.adminCommand({split: bucketsColl.getFullName(), middle: {[metaField]: 50}}));
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: bucketsColl.getFullName(),
                find: {[metaField]: 25},
                to: st.shard0.shardName,
                _waitForDelete: true,
            }),
        );
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: bucketsColl.getFullName(),
                find: {[metaField]: 75},
                to: st.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        for (let i = 0; i < 10; i++) {
            assert.commandWorked(
                mongosColl.insert({
                    [timeField]: ISODate("2025-09-03T10:00:00Z"),
                    [metaField]: i * 10,
                    v: i,
                }),
            );
        }
    });

    after(function () {
        st.stop();
    });

    it("explain path: rawData: false should produce TS_MODIFY plan, not DELETE", function () {
        // In raw-data mode the shard produces a DELETE plan (direct bucket delete) rather than TS_MODIFY (timeseries measurement delete). With rawData: false we should get a TS_MODIFY plan.
        const res = assert.commandWorked(
            mongosColl.runCommand({
                explain: {
                    findAndModify: mongosColl.getName(),
                    query: {[metaField]: {$gte: 20, $lte: 60}},
                    remove: true,
                    rawData: false,
                },
            }),
        );
        assert.eq(getWinningPlanFromExplain(res).stage, "TS_MODIFY", res);
    });

    it("explain path: rawData: false should produce the same plan as omitting rawData", function () {
        const omittedRawData = assert.commandWorked(
            mongosColl.runCommand({
                explain: {
                    findAndModify: mongosColl.getName(),
                    query: {[metaField]: {$gte: 20, $lte: 60}},
                    remove: true,
                },
            }),
        );
        const rawDataFalse = assert.commandWorked(
            mongosColl.runCommand({
                explain: {
                    findAndModify: mongosColl.getName(),
                    query: {[metaField]: {$gte: 20, $lte: 60}},
                    remove: true,
                    rawData: false,
                },
            }),
        );
        assert.eq(
            getWinningPlanFromExplain(rawDataFalse).stage,
            getWinningPlanFromExplain(omittedRawData).stage,
            "rawData: false and omitting rawData should produce the same plan stage",
        );
    });

    it("run path: findAndModify with rawData: false should match and remove a measurement", function () {
        // The query {meta: 20, v: 2} doesn't match any bucket document because 'v' is stored inside data.v (not at the top level), so
        // in raw data mode findAndModify would return null here. With rawData: false the measurement document should be found and returned.
        const result = assert.commandWorked(
            mongosDB.runCommand({
                findAndModify: mongosColl.getName(),
                query: {[metaField]: 20, v: 2},
                remove: true,
                rawData: false,
            }),
        );
        assert.neq(result.value, null, "Expected findAndModify to find and return the measurement document", {result});
        assert(
            result.value.hasOwnProperty(timeField),
            "Returned document should have the time field (measurement, not bucket)",
            {result},
        );
        assert(
            !result.value.hasOwnProperty("control"),
            "Returned document should not have a 'control' field (should be measurement, not raw bucket)",
            {result},
        );
    });
});
