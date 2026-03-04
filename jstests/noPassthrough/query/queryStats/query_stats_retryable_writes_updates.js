/**
 * Tests that query stats correctly handles retried update writes.
 *
 * Verifies that:
 * - A retried update (same lsid + txnNumber as an already-executed statement) does not record
 *     query stats for the retry attempt.
 * - A failed initial update attempt does not record query stats, but a successful retry does.
 * - A StaleConfig retry on a sharded cluster does not double-count query stats.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryStatsUpdateCmd, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

describe("query stats for retried update writes", function () {
    let rst;
    let primary;
    let testDB;
    let coll;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        testDB = primary.getDB("test");
        coll = testDB[collName];
    });

    after(function () {
        rst?.stopSet();
    });

    beforeEach(function () {
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {_id: 1, v: 1},
                {_id: 2, v: 2},
                {_id: 3, v: 3},
            ]),
        );
        resetQueryStatsStore(primary, "1MB");
    });

    it("retried already-executed statements in a batch should not record query stats", function () {
        const lsid = {id: UUID()};
        const txnNumber = NumberLong(1);

        // Execute a 2-statement batch (stmtIds 0 and 1).
        const firstCmd = {
            update: collName,
            updates: [
                {q: {_id: 1}, u: {$set: {v: 100}}, multi: false},
                {q: {_id: 2}, u: {$set: {v: 200}}, multi: false},
            ],
            lsid: lsid,
            txnNumber: txnNumber,
        };

        const firstResult = assert.commandWorked(testDB.runCommand(firstCmd));
        assert.eq(firstResult.nModified, 2);

        let entries = getQueryStatsUpdateCmd(primary, {collName: collName});
        assert.eq(entries.length, 1, "Expected 1 query stats entry after initial batch");
        assert.eq(entries[0].metrics.execCount, 2);

        // Re-send with the same lsid/txnNumber but a 3-statement batch.  StmtIds 0 and 1 are
        // already-executed retries; stmtId 2 is new.
        const retryCmd = {
            update: collName,
            updates: [
                {q: {_id: 1}, u: {$set: {v: 100}}, multi: false},
                {q: {_id: 2}, u: {$set: {v: 200}}, multi: false},
                {q: {_id: 3}, u: {$set: {v: 300}}, multi: false},
            ],
            lsid: lsid,
            txnNumber: txnNumber,
        };

        const retryResult = assert.commandWorked(testDB.runCommand(retryCmd));
        assert.eq(
            retryResult.retriedStmtIds,
            [0, 1],
            "Expected retriedStmtIds [0, 1]: " + tojson(retryResult.retriedStmtIds),
        );

        entries = getQueryStatsUpdateCmd(primary, {collName: collName});
        assert.eq(entries.length, 1, "Expected still 1 query stats entry after partial retry");
        assert.eq(entries[0].metrics.execCount, 3, "execCount should be 3 (2 original + 1 new; retries not counted)");
    });

    it("failed initial attempt should not record query stats; successful retry should", function () {
        const lsid = {id: UUID()};

        const updateCmd = {
            update: collName,
            updates: [{q: {_id: 2}, u: {$set: {v: 200}}, multi: false}],
            lsid: lsid,
            txnNumber: NumberLong(1),
        };

        // Fail the first update with a non-retryable error so the shell does not transparently
        // retry.  The failpoint intercepts before the command handler runs, so no query stats are
        // registered for the failed attempt.
        const fp = configureFailPoint(
            primary,
            "failCommand",
            {
                errorCode: ErrorCodes.OperationFailed,
                failCommands: ["update"],
                namespace: "test." + collName,
            },
            {times: 1},
        );

        assert.commandFailedWithCode(testDB.runCommand(updateCmd), ErrorCodes.OperationFailed);

        assert.eq(coll.findOne({_id: 2}).v, 2, "Document should not be modified after failed attempt");

        let entries = getQueryStatsUpdateCmd(primary, {collName: collName});
        assert.eq(entries.length, 0, "Expected no query stats after failed attempt");

        // The failpoint has expired (times: 1). Retry with the same lsid and txnNumber — the
        // server never executed the statement, so it will treat this as a fresh execution rather
        // than an already-executed retry.
        const retryResult = assert.commandWorked(testDB.runCommand(updateCmd));
        assert.eq(retryResult.nModified, 1);

        assert.eq(coll.findOne({_id: 2}).v, 200, "Document should be modified after successful retry");

        entries = getQueryStatsUpdateCmd(primary, {collName: collName});
        assert.eq(entries.length, 1, "Expected 1 query stats entry after successful retry");
        assert.eq(entries[0].metrics.execCount, 1);

        fp.off();
    });
});

describe("query stats for StaleConfig retried update (sharded)", function () {
    let st;
    let mongos;
    let testDB;
    let coll;
    let shard0Primary;

    before(function () {
        const queryStatsParams = {
            internalQueryStatsWriteCmdSampleRate: 1,
        };
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });

        mongos = st.s;
        testDB = mongos.getDB("test");

        assert.commandWorked(
            testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}),
        );

        coll = testDB[collName];
        assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(
            coll.insert([
                {_id: 1, v: 1},
                {_id: 2, v: 2},
            ]),
        );

        shard0Primary = st.rs0.getPrimary();
    });

    after(function () {
        st?.stop();
    });

    it("should record query stats once despite StaleConfig retry", function () {
        if (!st || !shard0Primary) {
            return;
        }

        resetQueryStatsStore(mongos, "1MB");
        resetQueryStatsStore(st.shard0, "1MB");

        // Force shard0 to return StaleConfig on the next metadata check, which triggers a
        // mongos-level retry.  The failpoint expires after one activation, so the retry succeeds.
        const fp = configureFailPoint(shard0Primary, "alwaysThrowStaleConfigInfo", {}, {times: 1});

        const result = assert.commandWorked(
            testDB.runCommand({
                update: collName,
                updates: [{q: {_id: 1}, u: {$set: {v: 100}}, multi: false}],
            }),
        );
        assert.eq(result.nModified, 1);
        assert.eq(coll.findOne({_id: 1}).v, 100);

        // Verify the failpoint actually fired (the update already completed, so
        // this returns immediately if the failpoint was hit).
        assert(fp.waitWithTimeout(1000), "alwaysThrowStaleConfigInfo failpoint was never triggered");

        // Mongos should show exactly 1 execution despite the internal retry.
        const mongosEntries = getQueryStatsUpdateCmd(mongos, {collName: collName});
        assert.eq(mongosEntries.length, 1, "Expected 1 mongos query stats entry: " + tojson(mongosEntries));
        assert.eq(mongosEntries[0].metrics.execCount, 1);

        // The shard should also show exactly 1 execution (the successful one).
        const shardEntries = getQueryStatsUpdateCmd(st.shard0, {collName: collName});
        assert.eq(shardEntries.length, 1, "Expected 1 shard query stats entry: " + tojson(shardEntries));
        assert.eq(shardEntries[0].metrics.execCount, 1);

        fp.off();
    });
});
