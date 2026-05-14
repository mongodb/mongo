/**
 * SERVER-122054 — regression test for collecting query stats on the standalone insert path.
 *
 * Re-lands the minimum guard that would have caught the wrapper-row contamination on the
 * no_passthrough_execution_control_with_prioritization variant before merge of #52339.
 *
 * Exercises:
 *   1. Single-doc insert on a standalone: $queryStats has command: "insert" + non-null
 *      queryShapeHash; that hash equals the queryShapeHash in the mongod slow query log.
 *   2. Multi-doc batch insert: one $queryStats row (one execCount bump), writes.nInserted == N.
 *   3. Feature-flag off ⇒ no $queryStats rows for inserts.
 *   4. Wrapper-isolation: when an update command (which can emit a wrapper slow-log line
 *      without a queryShapeHash on certain variants) executes alongside inserts in the same
 *      session, getSlowQueryLogs({commandType: "insert"}) MUST NOT return the update wrapper.
 *
 * Pre-revert root cause this guards: getSlowQueryLogs widened its commandType matcher to
 * also match command-level slow log lines (inserts/finds), but did not exclude write-command
 * wrapper lines that lack queryShapeHash. Tests like update_cmd_mongod_slow_query_log.js then
 * tripped getQueryShapeHashSetFromSlowLogs' non-null assertion. The helper fix is to drop
 * commandType-matched rows whose attr.queryShapeHash is undefined; this test pins that
 * behaviour from the insert side.
 *
 * @tags: [requires_fcv_90, featureFlagQueryStatsInsert]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getQueryShapeHashSetFromSlowLogs,
    getSlowQueryLogs,
} from "jstests/libs/query/query_stats_utils.js";

const kCollName = "insert_stats_standalone";
const kFeatureFlag = "featureFlagQueryStatsInsert";

/**
 * Returns every $queryStats entry whose key namespace matches collName AND whose key
 * carries command: "insert". Sorted by latestSeenTimestamp DESC.
 */
function getInsertQueryStatsEntries(conn, dbName, collName) {
    const adminDB = conn.getDB("admin");
    const ns = `${dbName}.${collName}`;
    return adminDB
        .aggregate([
            {$queryStats: {}},
            {
                $match: {
                    "key.queryShape.cmdNs.db": dbName,
                    "key.queryShape.cmdNs.coll": collName,
                    "key.queryShape.command": "insert",
                },
            },
            {$sort: {"metrics.latestSeenTimestamp": -1}},
        ])
        .toArray()
        .filter((entry) => entry.key.queryShape.cmdNs.db === dbName);
}

function startStandaloneWithInsertStats({featureFlagOn = true} = {}) {
    const params = {
        internalQueryStatsRateLimit: -1,
        internalQueryStatsWriteCmdSampleRate: 1,
        // Force every command into the slow-query log so the helper has rows to inspect.
        slowms: -1,
    };
    if (featureFlagOn) {
        params[kFeatureFlag] = true;
    }
    const conn = MongoRunner.runMongod({setParameter: params});
    assert.neq(conn, null, "failed to spin up standalone mongod for SERVER-122054 regression");
    return conn;
}

describe("SERVER-122054 standalone insert query-stats wiring", function () {
    describe("with featureFlagQueryStatsInsert ON", function () {
        before(function () {
            this.conn = startStandaloneWithInsertStats({featureFlagOn: true});
            this.dbName = jsTestName();
            this.testDB = this.conn.getDB(this.dbName);
            this.testDB.setProfilingLevel(0, -1);
        });

        after(function () {
            MongoRunner.stopMongod(this.conn);
        });

        beforeEach(function () {
            this.testDB[kCollName].drop();
            // Pre-create so the first insert isn't a create-on-first-write — keeps the
            // InsertKey's collectionType stable at kCollection from the first call.
            assert.commandWorked(this.testDB.createCollection(kCollName));
        });

        it("single-doc insert: $queryStats entry has command:insert + non-null queryShapeHash", function () {
            const comment = `insert_single_${UUID().toString()}`;

            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: [{a: 1, b: "hello"}],
                    comment: comment,
                }),
            );

            const entries = getInsertQueryStatsEntries(this.conn, this.dbName, kCollName);
            assert.gte(entries.length, 1, "expected at least one $queryStats entry for the insert");

            const entry = entries[0];
            assert.eq(
                entry.key.queryShape.command,
                "insert",
                `unexpected command field in shape: ${tojson(entry.key.queryShape)}`,
            );
            assert.neq(
                entry.queryShapeHash,
                null,
                `expected non-null queryShapeHash on insert entry: ${tojson(entry)}`,
            );
            assert.gte(entry.metrics.execCount, 1, "execCount should bump on insert");

            // Mongod slow log must carry the same queryShapeHash for this command.
            const slowLogHash = getQueryShapeHashFromSlowLogs({
                testDB: this.testDB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(slowLogHash, null, "queryShapeHash missing from mongod slow log for insert");
            assert.eq(
                slowLogHash,
                entry.queryShapeHash,
                "queryShapeHash mismatch between $queryStats and mongod slow log",
            );
        });

        it("multi-doc batch insert: one execCount bump, writes.nInserted == N", function () {
            const comment = `insert_multi_${UUID().toString()}`;
            const docs = [
                {v: 1, payload: "alpha"},
                {v: 2, payload: "beta"},
                {v: 3, payload: "gamma"},
                {v: 4, payload: "delta"},
                {v: 5, payload: "epsilon"},
            ];

            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: docs,
                    ordered: true,
                    comment: comment,
                }),
            );

            const entries = getInsertQueryStatsEntries(this.conn, this.dbName, kCollName);
            assert.eq(
                entries.length,
                1,
                `multi-doc insert should produce exactly one $queryStats row, got ${entries.length}: ${tojson(entries)}`,
            );

            const entry = entries[0];
            assert.eq(entry.metrics.execCount, 1, "execCount should bump once per command, not per document");
            assert.eq(
                entry.metrics.writes && entry.metrics.writes.nInserted,
                docs.length,
                `writes.nInserted should equal ${docs.length}: ${tojson(entry.metrics)}`,
            );
            assert.neq(entry.queryShapeHash, null, "queryShapeHash should be present on batch insert");

            // Slow-log set must contain exactly the single shape hash (helper must not return wrappers).
            const slowLogHashes = getQueryShapeHashSetFromSlowLogs({
                testDB: this.testDB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.eq(
                slowLogHashes.size,
                1,
                `expected exactly one insert hash in slow logs, got ${slowLogHashes.size}: ` +
                    `[${Array.from(slowLogHashes)}]`,
            );
            assert(
                slowLogHashes.has(entry.queryShapeHash),
                `slow-log set did not contain the $queryStats hash ${entry.queryShapeHash}: ` +
                    `[${Array.from(slowLogHashes)}]`,
            );
        });

        it("shape invariance: different document content collapses to one shape", function () {
            const commentA = `insert_shape_invariance_a_${UUID().toString()}`;
            const commentB = `insert_shape_invariance_b_${UUID().toString()}`;

            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: [{x: 1}],
                    comment: commentA,
                }),
            );
            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: [{y: "totally different doc"}, {z: [1, 2, 3]}],
                    comment: commentB,
                }),
            );

            const entries = getInsertQueryStatsEntries(this.conn, this.dbName, kCollName);
            // The documents field is shapified as a placeholder, so all three inserts share one shape.
            const hashes = new Set(entries.map((e) => e.queryShapeHash));
            assert.eq(
                hashes.size,
                1,
                `expected one shared queryShapeHash across content-varying inserts, got ${hashes.size}: ` +
                    `${tojson(entries.map((e) => ({hash: e.queryShapeHash, exec: e.metrics.execCount})))}`,
            );
        });

        it("wrapper-isolation: getSlowQueryLogs(commandType:insert) excludes hash-less wrappers", function () {
            // This is the targeted regression for the auto-revert. Even when other commands in the
            // session log wrapper-style entries without a queryShapeHash, the slow-log helper used
            // by the insert tests must only return command-level entries that carry the hash.
            const insertComment = `wrapper_isolation_insert_${UUID().toString()}`;

            assert.commandWorked(this.testDB[kCollName].insertOne({seed: 1}));
            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: [{a: 1}, {a: 2}],
                    comment: insertComment,
                }),
            );

            // Drive an update on the same DB to force the kind of wrapper-vs-nested-statement
            // dual emission seen on the prioritization variant.
            assert.commandWorked(
                this.testDB.runCommand({
                    update: kCollName,
                    updates: [{q: {a: 1}, u: {$set: {touched: true}}}],
                    comment: `wrapper_isolation_update_${UUID().toString()}`,
                }),
            );

            const rawInsertLogs = getSlowQueryLogs(this.testDB, insertComment, {commandType: "insert"});
            assert.gte(rawInsertLogs.length, 1, "expected at least one insert slow-log row");
            for (const log of rawInsertLogs) {
                assert.neq(
                    log.attr.queryShapeHash,
                    undefined,
                    `getSlowQueryLogs returned an insert-typed row without queryShapeHash — wrapper leak: ${tojson(log.attr)}`,
                );
                assert.neq(
                    log.attr.queryShapeHash,
                    null,
                    `getSlowQueryLogs returned an insert-typed row with null queryShapeHash: ${tojson(log.attr)}`,
                );
            }
        });
    });

    describe("with featureFlagQueryStatsInsert OFF", function () {
        before(function () {
            this.conn = startStandaloneWithInsertStats({featureFlagOn: false});
            this.dbName = jsTestName() + "_flag_off";
            this.testDB = this.conn.getDB(this.dbName);
            this.testDB.setProfilingLevel(0, -1);
        });

        after(function () {
            MongoRunner.stopMongod(this.conn);
        });

        it("produces no $queryStats rows for inserts when the feature flag is off", function () {
            this.testDB[kCollName].drop();
            assert.commandWorked(this.testDB.createCollection(kCollName));

            assert.commandWorked(
                this.testDB.runCommand({
                    insert: kCollName,
                    documents: [{a: 1}, {a: 2}, {a: 3}],
                    comment: `flag_off_${UUID().toString()}`,
                }),
            );

            const entries = getInsertQueryStatsEntries(this.conn, this.dbName, kCollName);
            assert.eq(
                entries.length,
                0,
                `expected zero insert $queryStats rows with flag off, got: ${tojson(entries)}`,
            );
        });
    });
});
