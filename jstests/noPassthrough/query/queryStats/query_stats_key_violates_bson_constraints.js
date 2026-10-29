/**
 * Integration coverage for $queryStats when a command's query shape approaches or exceeds the BSON
 * size or nesting depth limits.
 *
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStats,
    resetQueryStatsStore,
    getQueryStatsWithTransform,
} from "jstests/libs/query/query_stats_utils.js";

describe("$queryStats and query shapes near the BSON limits", function () {
    // Each clause '{x: {$lt: i, $gte: i}}' shapifies into two predicates on 'x'. ~226000 clauses
    // produce a shapified query above BSONObjMaxInternalSize (16MB + 16KB).
    const kOversizedClauses = 226000;

    // Pinned via setParameter below so the depth arithmetic does not drift with the server default.
    const kMaxBSONDepth = 200;

    // A dotted projection path shapifies into one nested object per component. The stored key adds
    // 4 levels above it ('queryShape.pipeline.<n>.$project') and the reply adds 4 more
    // ('cursor.firstBatch.<n>.key'). This length keeps the key valid on its own (199 <=
    // kMaxBSONDepth) while making it unreturnable (203 > kMaxBSONDepth).
    const kTooDeepPath = kMaxBSONDepth - 5;

    const kShallowPath = 5;

    function buildLargeAndFilter(numClauses) {
        const clauses = [];
        for (let i = 1; i <= numClauses; i++) {
            clauses.push({x: {$lt: i, $gte: i}});
        }
        return {$and: clauses};
    }

    function buildDottedPath(depth) {
        let path = "a0";
        for (let i = 1; i < depth; i++) {
            path += ".a" + i;
        }
        return path;
    }

    function aggregateWithProjectDepth(depth) {
        assert.commandWorked(
            testDB.runCommand({
                aggregate: collName,
                pipeline: [{$project: {[buildDottedPath(depth)]: 1}}],
                cursor: {},
            }),
        );
    }

    let conn, testDB, collName;

    before(function () {
        // Sample every read and write so all command types are collected.
        conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryStatsSampleRate: 1,
                internalQueryStatsWriteCmdSampleRate: 1,
                maxBSONDepth: kMaxBSONDepth,
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

    it("collects an aggregate whose projection depth is under the limit", function () {
        aggregateWithProjectDepth(kShallowPath);
        const entries = getQueryStats(conn, {collName});
        assert.eq(entries.length, 1, entries);
    });

    it("does not fail $queryStats but omits an entry too deeply nested for the reply", function () {
        const errorsBefore = testDB.serverStatus().metrics.queryStats.numHmacApplicationErrors;

        aggregateWithProjectDepth(kTooDeepPath);

        // Reading query stats must succeed rather than returning a reply no client can parse.
        const entries = getQueryStats(conn, {collName});
        assert.eq(entries, [], entries);

        // Proves entry was skipped as numHmacApplicationErrors is incremented on the query stats
        // read path.
        assert.eq(
            testDB.serverStatus().metrics.queryStats.numHmacApplicationErrors,
            errorsBefore + 1,
        );
    });

    it("returns the remaining entries alongside one that is too deeply nested", function () {
        aggregateWithProjectDepth(kTooDeepPath);
        assert.commandWorked(testDB.runCommand({find: collName, filter: {x: 1}}));

        const entries = getQueryStats(conn, {collName});
        assert.eq(entries.length, 1, entries);
        assert.eq(entries[0].key.queryShape.command, "find", entries);
    });
});
