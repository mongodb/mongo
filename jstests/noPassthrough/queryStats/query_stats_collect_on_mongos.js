/**
 * Test that mongos is collecting query stats metrics.
 * @tags: [featureFlagQueryStats]
 */

load('jstests/libs/query_stats_utils.js');

(function() {
"use strict";

const setup = () => {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        config: 1,
        rs: {nodes: 1},
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
                'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
            }
        },
    });
    const mongos = st.s;
    const db = mongos.getDB("test");
    const coll = db.coll;
    coll.insert({v: 1});
    coll.insert({v: 4});
    return st;
};

const assertExpectedResults = (results,
                               expectedQueryStatsKey,
                               expectedExecCount,
                               expectedDocsReturnedSum,
                               expectedDocsReturnedMax,
                               expectedDocsReturnedMin,
                               expectedDocsReturnedSumOfSq,
                               getMores) => {
    const {key, metrics} = results;
    confirmAllExpectedFieldsPresent(expectedQueryStatsKey, key);
    assert.eq(expectedExecCount, metrics.execCount);
    assert.docEq({
        sum: NumberLong(expectedDocsReturnedSum),
        max: NumberLong(expectedDocsReturnedMax),
        min: NumberLong(expectedDocsReturnedMin),
        sumOfSquares: NumberLong(expectedDocsReturnedSumOfSq)
    },
                 metrics.docsReturned);

    const {
        firstSeenTimestamp,
        latestSeenTimestamp,
        lastExecutionMicros,
        totalExecMicros,
        firstResponseExecMicros
    } = metrics;

    // This test can't predict exact timings, so just assert these three fields have been set (are
    // non-zero).
    assert.neq(lastExecutionMicros, NumberLong(0));
    assert.neq(firstSeenTimestamp.getTime(), 0);
    assert.neq(latestSeenTimestamp.getTime(), 0);

    const distributionFields = ['sum', 'max', 'min', 'sumOfSquares'];
    for (const field of distributionFields) {
        assert.neq(totalExecMicros[field], NumberLong(0));
        assert.neq(firstResponseExecMicros[field], NumberLong(0));
        if (getMores) {
            // If there are getMore calls, totalExecMicros fields should be greater than or equal to
            // firstResponseExecMicros.
            if (field == 'min' || field == 'max') {
                // In the case that we've executed multiple queries with the same shape, it is
                // possible for the min or max to be equal.
                assert.gte(totalExecMicros[field], firstResponseExecMicros[field]);
            } else {
                // TODO SERVER-78983: Renable this assert once queryStats key hash persists through
                // ClusterFind::runGetMore.
                // assert.gt(totalExecMicros[field], firstResponseExecMicros[field]);
            }
        } else {
            // If there are no getMore calls, totalExecMicros fields should be equal to
            // firstResponseExecMicros.
            assert.eq(totalExecMicros[field], firstResponseExecMicros[field]);
        }
    }
};

// Assert that, for find queries, no query stats results are written until a cursor has reached
// exhaustion; ensure accurate results once they're written.
{
    const st = setup();
    const db = st.s.getDB("test");
    const collName = "coll";
    const coll = db[collName];

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "find",
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
        },
        readConcern: {level: "local", provenance: "implicitDefault"},
        batchSize: "?number",
        client: {application: {name: "MongoDB Shell"}}
    };

    const cursor = coll.find({v: {$gt: 0, $lt: 5}}).batchSize(1);  // returns 1 doc

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db);
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(db.runCommand({
        getMore: cursor.getId(),
        collection: coll.getName(),
        batchSize: 2
    }));  // returns 1 doc, exhausts the cursor
    queryStats = getQueryStatsFindCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4,
                          /* getMores */ true);

    // Run more queries (to exhaustion) with the same query shape, and ensure query stats results
    // are accurate.
    coll.find({v: {$gt: 2, $lt: 3}}).batchSize(10).toArray();  // returns 0 docs
    coll.find({v: {$gt: 0, $lt: 1}}).batchSize(10).toArray();  // returns 0 docs
    coll.find({v: {$gt: 0, $lt: 2}}).batchSize(10).toArray();  // return 1 doc
    queryStats = getQueryStatsFindCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 4,
                          /* expectedDocsReturnedSum */ 3,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 0,
                          /* expectedDocsReturnedSumOfSq */ 5,
                          /* getMores */ true);

    st.stop();
}

// Assert that, for agg queries, no query stats results are written until a cursor has reached
// exhaustion; ensure accurate results once they're written.
{
    const st = setup();
    const db = st.s.getDB("test");
    const coll = db.coll;

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "aggregate",
            pipeline: [
                {$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}},
                {$project: {_id: true, hello: true}}
            ]

        },
        cursor: {batchSize: "?number"},
        applicationName: "MongoDB Shell",
    };

    const cursor = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 5}}},
            {$project: {hello: true}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc

    // Since the cursor hasn't been exhausted yet, ensure no query stats results have been written
    // yet.
    let queryStats = getQueryStats(db);
    assert.eq(0, queryStats.length, queryStats);

    // Run a getMore to exhaust the cursor, then ensure query stats results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(db.runCommand({
        getMore: cursor.getId(),
        collection: coll.getName(),
        batchSize: 2
    }));  // returns 1 doc, exhausts the cursor
    queryStats = getQueryStatsAggCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4,
                          /* getMores */ true);

    // Run more queries (to exhaustion) with the same query shape, and ensure query stats results
    // are accurate.
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 5}}},
        {$project: {hello: true}},
    ]);  // returns 2 docs
    coll.aggregate([
        {$match: {v: {$gt: 2, $lt: 3}}},
        {$project: {hello: true}},
    ]);  // returns 0 docs
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 2}}},
        {$project: {hello: true}},
    ]);  // returns 1 doc
    queryStats = getQueryStatsAggCmd(db);
    assert.eq(1, queryStats.length, queryStats);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 4,
                          /* expectedDocsReturnedSum */ 5,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 0,
                          /* expectedDocsReturnedSumOfSq */ 9,
                          /* getMores */ true);

    st.stop();
}

// Assert on batchSize-limited find queries that killCursors will write metrics with partial results
// to the query stats store.
{
    const st = setup();
    const db = st.s.getDB("test");
    const collName = "coll";
    const coll = db[collName];

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "find",
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
        },
        readConcern: {level: "local", provenance: "implicitDefault"},
        batchSize: "?number",
        client: {application: {name: "MongoDB Shell"}}
    };

    const cursor1 = coll.find({v: {$gt: 0, $lt: 5}}).batchSize(1);  // returns 1 doc
    const cursor2 = coll.find({v: {$gt: 0, $lt: 2}}).batchSize(1);  // returns 1 doc

    assert.commandWorked(
        db.runCommand({killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));
    const queryStats = getQueryStats(db);
    assert.eq(1, queryStats.length);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 2,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 2,
                          /* getMores */ false);
    st.stop();
}

// Assert on batchSize-limited agg queries that killCursors will write metrics with partial results
// to the query stats store.
{
    const st = setup();
    const db = st.s.getDB("test");
    const coll = db.coll;

    const queryStatsKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            command: "aggregate",
            pipeline: [{$match: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]}}]
        },
        cursor: {batchSize: "?number"},
        applicationName: "MongoDB Shell",
    };

    const cursor1 = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 5}}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc
    const cursor2 = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 2}}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc

    assert.commandWorked(
        db.runCommand({killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));
    const queryStats = getQueryStats(db);
    assert.eq(1, queryStats.length);
    assertExpectedResults(queryStats[0],
                          queryStatsKey,
                          /* expectedExecCount */ 2,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 2,
                          /* getMores */ false);
    st.stop();
}
}());
