/**
 * Test $queryStats behavior for $vectorSearch.
 *
 * Note that this test does not actually require talking to mongot at all because we can test query
 * stats on a nonexistent collection. We use the mock framework because we need to override a server
 * parameter and test on different topologies.
 */
import {getQueryStats, getValueAtPath} from "jstests/libs/query/query_stats_utils.js";
import {
    MongotMock,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

function runTest(conn) {
    // Use an empty collection so that we don't have to mock responses. Query stats will still
    // collect full statistics for everything that we care about in this test.
    const testDB = conn.getDB(dbName);
    const coll = testDB[collName];

    // Two queries that should have the same query shape, but different vector search metrics. The
    // only components that affect the shape are 'filter' and 'index', and the only ones reported in
    // the metrics are 'limit' and 'numCandidates'.
    coll.aggregate({
            $vectorSearch: {
                filter: {a: {$gt: 5}},
                index: "index_1",
                path: "x",
                queryVector: [1.0, 2.0, 3.0],
                limit: 5,
                numCandidates: 10,
            }
        })
        .toArray();
    coll.aggregate({
            $vectorSearch: {
                filter: {a: {$gt: 5}},
                index: "index_1",
                limit: 10,
                numCandidates: 10,
            }
        })
        .toArray();

    // Different shape (no index name).
    coll.aggregate({
            $vectorSearch: {
                filter: {a: {$gt: 5}},
                limit: 4,
                numCandidates: 6,
            }
        })
        .toArray();

    // Empty shape. This is not a valid vector search query, but mongod should not validate that
    // (it's mongot's responsibility).
    coll.aggregate({$vectorSearch: {}}).toArray();

    const stats = getQueryStats(conn, {collName});
    assert.eq(stats.length, 3, stats);

    {
        // Third shape, with one execution.
        const entry = stats[0];
        assert.docEq({$vectorSearch: {}}, entry.key.queryShape.pipeline[0], entry);
        assert.eq(entry.metrics.execCount, 1, entry);

        // Metrics should be 0 because this query did not have numeric 'limit'/'numCandidates'
        // fields.
        const vectorSearchMetrics =
            getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        assert.docEq({sum: 0, min: 0, max: 0, sumOfSquares: 0}, vectorSearchMetrics.limit, entry);
        assert.docEq({sum: 0, min: 0, max: 0, sumOfSquares: 0},
                     vectorSearchMetrics.numCandidatesLimitRatio,
                     entry);
    }

    {
        // Second shape, with one execution.
        const entry = stats[1];
        assert.docEq({$vectorSearch: {filter: {a: {$gt: "?number"}}}},
                     entry.key.queryShape.pipeline[0],
                     entry);
        assert.eq(entry.metrics.execCount, 1, entry);

        const vectorSearchMetrics =
            getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        assert.docEq({sum: 4, min: 4, max: 4, sumOfSquares: 16}, vectorSearchMetrics.limit, entry);
        assert.docEq({sum: 1.5, min: 1.5, max: 1.5, sumOfSquares: 2.25},
                     vectorSearchMetrics.numCandidatesLimitRatio,
                     entry);
    }

    {
        // First shape, with two executions.
        const entry = stats[2];
        assert.docEq({$vectorSearch: {filter: {a: {$gt: "?number"}}, index: "index_1"}},
                     entry.key.queryShape.pipeline[0],
                     entry);
        assert.eq(entry.metrics.execCount, 2, entry);

        const vectorSearchMetrics =
            getValueAtPath(entry.metrics, "supplementalMetrics.vectorSearch");
        assert.docEq(
            {sum: 15, min: 5, max: 10, sumOfSquares: 125}, vectorSearchMetrics.limit, entry);
        assert.docEq({sum: 3, min: 1, max: 2, sumOfSquares: 5},
                     vectorSearchMetrics.numCandidatesLimitRatio,
                     entry);
    }
}

{
    // Start mock mongot.
    const mongotMock = new MongotMock();
    mongotMock.start();
    const mockConn = mongotMock.getConnection();

    // Start mongod.
    const conn = MongoRunner.runMongod({
        setParameter: {mongotHost: mockConn.host, internalQueryStatsRateLimit: -1},
        verbose: 1,
    });

    runTest(conn);

    mongotMock.stop();
    MongoRunner.stopMongod(conn);
}

{
    const stWithMock = new ShardingTestWithMongotMock({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
        }
    });
    stWithMock.start();

    runTest(stWithMock.st.s);

    stWithMock.stop();
}