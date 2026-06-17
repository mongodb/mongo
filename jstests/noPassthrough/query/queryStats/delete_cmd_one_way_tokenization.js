/**
 * Test that $queryStats properly tokenizes delete commands on mongod and mongos.
 *
 * @tags: [featureFlagQueryStatsDelete]
 */
import {it} from "jstests/libs/mochalite.js";
import {
    getQueryStatsDeleteCmd,
    kHashedFieldName,
    kHashedIdField,
} from "jstests/libs/query/query_stats_utils.js";
import {runTokenizationTestsForTopology} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

function deleteTokenizationTests(ctxFn) {
    it("should tokenize single delete filter correctly", function () {
        const {testDB} = ctxFn();
        assert.commandWorked(
            testDB.runCommand({
                delete: collName,
                deletes: [{q: {v: 1}, limit: 1}],
                comment: "single delete!",
            }),
        );

        let queryStats = getQueryStatsDeleteCmd(testDB, {transformIdentifiers: true});
        assert.eq(1, queryStats.length);
        assert.eq("delete", queryStats[0].key.queryShape.command);
        assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
        assert.eq(1, queryStats[0].key.queryShape.limit);
    });

    // Simple _id queries use the IDHACK path and skip parsing, but should still be
    // recorded in query stats with proper tokenization.
    it("should tokenize simple _id delete filter correctly", function () {
        const {testDB, coll} = ctxFn();
        assert.commandWorked(coll.insert({_id: 123, v: 99}));
        assert.commandWorked(
            testDB.runCommand({
                delete: collName,
                deletes: [{q: {_id: 123}, limit: 1}],
                comment: "delete filtered on _id!",
            }),
        );

        let queryStats = getQueryStatsDeleteCmd(testDB, {transformIdentifiers: true});
        assert.eq(1, queryStats.length);
        assert.eq("delete", queryStats[0].key.queryShape.command);
        assert.eq({[kHashedIdField]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
        assert.eq(1, queryStats[0].key.queryShape.limit);
    });

    it("should tokenize multi delete filter correctly", function () {
        const {testDB} = ctxFn();
        assert.commandWorked(
            testDB.runCommand({
                delete: collName,
                deletes: [{q: {v: {$gt: 1}}, limit: 0}],
                comment: "multi delete!",
            }),
        );

        let queryStats = getQueryStatsDeleteCmd(testDB, {transformIdentifiers: true});
        assert.eq(1, queryStats.length);
        assert.eq("delete", queryStats[0].key.queryShape.command);
        assert.eq({[kHashedFieldName]: {$gt: "?number"}}, queryStats[0].key.queryShape.q);
        assert.eq(0, queryStats[0].key.queryShape.limit);
    });

    it("should tokenize delete with complex filter and let correctly", function () {
        const {testDB} = ctxFn();
        // Hash of "targetVal"
        const kHashedLetVar = "+GVDV+bYqLCJTrG/ajkhEEzT5xDLga/sH6VqrCy4pog=";
        assert.commandWorked(
            testDB.runCommand({
                delete: collName,
                deletes: [{q: {$or: [{v: {$lt: "$$targetVal"}}, {v: {$gt: 5}}]}, limit: 0}],
                let: {targetVal: 2},
                comment: "delete with $or filter and let!",
            }),
        );

        let queryStats = getQueryStatsDeleteCmd(testDB, {transformIdentifiers: true});
        assert.eq(1, queryStats.length);
        assert.eq("delete", queryStats[0].key.queryShape.command);
        // "$$targetVal" in the filter is a string literal — shapified to "?string".
        assert.eq(
            {$or: [{[kHashedFieldName]: {$lt: "?string"}}, {[kHashedFieldName]: {$gt: "?number"}}]},
            queryStats[0].key.queryShape.q,
        );
        assert.eq({[kHashedLetVar]: "?number"}, queryStats[0].key.queryShape.let);
        assert.eq(0, queryStats[0].key.queryShape.limit);
    });
}

runTokenizationTestsForTopology(
    "delete command one way tokenization (Standalone)",
    () => {
        const conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryStatsWriteCmdSampleRate: 1,
            },
        });
        return {fixture: conn, testDB: conn.getDB("test")};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
    {collName, initialDocs: [{v: 1}, {v: 2}, {v: 3}]},
    deleteTokenizationTests,
);

runTokenizationTestsForTopology(
    "delete command one way tokenization (Sharded)",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
        });
        const testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
        return {fixture: st, testDB};
    },
    (st) => st.stop(),
    {collName, initialDocs: [{v: 1}, {v: 2}, {v: 3}]},
    deleteTokenizationTests,
);
