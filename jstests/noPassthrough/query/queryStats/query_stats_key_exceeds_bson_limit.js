/**
 * Integration coverage for $queryStats when a command's query shape approaches or exceeds the 16MB
 * BSON limits.
 *
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStats,
    resetQueryStatsStore,
    getQueryStatsWithTransform,
} from "jstests/libs/query/query_stats_utils.js";

describe("$queryStats and query shapes near the 16MB BSON limit", function () {
    // Each clause '{x: {$lt: i, $gte: i}}' shapifies into two predicates on 'x'. ~226000 clauses
    // produce a shapified query above BSONObjMaxInternalSize (16MB + 16KB).
    const kOversizedClauses = 226000;

    function buildLargeAndFilter(numClauses) {
        const clauses = [];
        for (let i = 1; i <= numClauses; i++) {
            clauses.push({x: {$lt: i, $gte: i}});
        }
        return {$and: clauses};
    }

    let conn, testDB, collName;

    before(function () {
        // Sample every read and write so all command types are collected.
        conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryStatsSampleRate: 1,
                internalQueryStatsWriteCmdSampleRate: 1,
            },
        });
        testDB = conn.getDB("test");
        collName = jsTestName();
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    beforeEach(function () {
        const coll = testDB[collName];
        coll.drop();
        assert.commandWorked(coll.insert({x: 1}));
        // Clear any entries so each test observes only its own command.
        resetQueryStatsStore(conn, "1%");
    });

    it(`collects a find whose query shape is under the limit`, function () {
        assert.commandWorked(testDB.runCommand({find: collName, filter: {x: 1}}));
        const entries = getQueryStats(conn, {collName});
        assert.eq(entries.length, 1, entries);
    });

    it(`does not collect a find whose query shape exceeds BSONObjMaxInternalSize`, function () {
        // The command succeeds, query stats registration is skipped when the shape is too
        // large, so no entry is collected and $queryStats still succeeds.
        const errorsBefore = testDB.serverStatus().metrics.queryStats.numQueryStatsStoreWriteErrors;

        assert.commandWorked(
            testDB.runCommand({find: collName, filter: buildLargeAndFilter(kOversizedClauses)}),
        );
        const entries = getQueryStats(conn, {collName});
        assert.eq(entries.length, 0, entries);

        // We increment the error metric when the query shape is too large.
        assert.eq(
            testDB.serverStatus().metrics.queryStats.numQueryStatsStoreWriteErrors,
            errorsBefore + 1,
        );
    });

    it("does not fail $queryStats but omits an entry whose HMAC key exceeds 16MB", function () {
        // ~120000 clauses -> representative shape ~9MB (stored), HMAC-transformed key ~21MB.
        assert.commandWorked(
            testDB.runCommand({find: collName, filter: buildLargeAndFilter(120000)}),
        );

        // Without HMAC the entry is returned.
        assert.eq(getQueryStats(conn, {collName}).length, 1);

        const errorsBefore = testDB.serverStatus().metrics.queryStats.numHmacApplicationErrors;

        // With HMAC the command should still succeed, just without this entry.
        const result = getQueryStatsWithTransform(
            conn,
            {},
            {collName: collName, transformIdentifiers: true},
        );
        assert.eq(result, [], result);

        // Assert the error metric was incremented.
        assert.eq(
            testDB.serverStatus().metrics.queryStats.numHmacApplicationErrors,
            errorsBefore + 1,
        );
    });
});
