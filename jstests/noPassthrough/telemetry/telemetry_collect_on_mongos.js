/**
 * Test that mongos is collecting telemetry metrics.
 * @tags: [featureFlagTelemetry]
 */

load('jstests/libs/telemetry_utils.js');

(function() {
"use strict";

// Redacted literal replacement string. This may change in the future, so it's factored out.
const aggRedactString = "###";
const setup = () => {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        config: 1,
        rs: {nodes: 1},
        mongosOptions: {
            setParameter: {
                internalQueryConfigureTelemetrySamplingRate: -1,
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
                               expectedTelemetryKey,
                               expectedExecCount,
                               expectedDocsReturnedSum,
                               expectedDocsReturnedMax,
                               expectedDocsReturnedMin,
                               expectedDocsReturnedSumOfSq) => {
    const {key, metrics} = results;
    assert.eq(expectedTelemetryKey, key);
    assert.eq(expectedExecCount, metrics.execCount);
    assert.docEq({
        sum: NumberLong(expectedDocsReturnedSum),
        max: NumberLong(expectedDocsReturnedMax),
        min: NumberLong(expectedDocsReturnedMin),
        sumOfSquares: NumberLong(expectedDocsReturnedSumOfSq)
    },
                 metrics.docsReturned);

    // This test can't predict exact timings, so just assert these three fields have been set (are
    // non-zero).
    const {firstSeenTimestamp, lastExecutionMicros, queryExecMicros} = metrics;

    assert.neq(timestampCmp(firstSeenTimestamp, Timestamp(0, 0)), 0);
    assert.neq(lastExecutionMicros, NumberLong(0));

    const distributionFields = ['sum', 'max', 'min', 'sumOfSquares'];
    for (const field of distributionFields) {
        assert.neq(queryExecMicros[field], NumberLong(0));
    }
};

// Assert that, for find queries, no telemetry results are written until a cursor has reached
// exhaustion; ensure accurate results once they're written.
{
    const st = setup();
    const db = st.s.getDB("test");
    const collName = "coll";
    const coll = db[collName];

    const telemetryKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            find: collName,
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
            batchSize: "?number",
        },
        readConcern: {level: "local", provenance: "implicitDefault"},
        applicationName: "MongoDB Shell",
    };

    const cursor = coll.find({v: {$gt: 0, $lt: 5}}).batchSize(1);  // returns 1 doc

    // Since the cursor hasn't been exhausted yet, ensure no telemetry results have been written
    // yet.
    let telemetry = getTelemetry(db);
    assert.eq(0, telemetry.length);

    // Run a getMore to exhaust the cursor, then ensure telemetry results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(db.runCommand({
        getMore: cursor.getId(),
        collection: coll.getName(),
        batchSize: 2
    }));  // returns 1 doc, exhausts the cursor
    // The $telemetry query for the previous `getTelemetry` is included in this call to $telemetry.
    telemetry = getTelemetry(db);
    assert.eq(2, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4);

    // Run more queries (to exhaustion) with the same query shape, and ensure telemetry results are
    // accurate.
    coll.find({v: {$gt: 2, $lt: 3}}).batchSize(10).toArray();  // returns 0 docs
    coll.find({v: {$gt: 0, $lt: 1}}).batchSize(10).toArray();  // returns 0 docs
    coll.find({v: {$gt: 0, $lt: 2}}).batchSize(10).toArray();  // return 1 doc
    telemetry = getTelemetry(db);
    assert.eq(2, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 4,
                          /* expectedDocsReturnedSum */ 3,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 0,
                          /* expectedDocsReturnedSumOfSq */ 5);

    st.stop();
}

// Assert that, for agg queries, no telemetry results are written until a cursor has reached
// exhaustion; ensure accurate results once they're written.
{
    const st = setup();
    const db = st.s.getDB("test");
    const coll = db.coll;

    const telemetryKey = {
        pipeline: [
            {$match: {v: {$gt: aggRedactString, $lt: aggRedactString}}},
            {$project: {hello: aggRedactString}},
        ],
        namespace: "test.coll",
        applicationName: "MongoDB Shell"
    };

    const cursor = coll.aggregate(
        [
            {$match: {v: {$gt: 0, $lt: 5}}},
            {$project: {hello: "$world"}},
        ],
        {cursor: {batchSize: 1}});  // returns 1 doc

    // Since the cursor hasn't been exhausted yet, ensure no telemetry results have been written
    // yet.
    let telemetry = getTelemetry(db);
    assert.eq(0, telemetry.length);

    // Run a getMore to exhaust the cursor, then ensure telemetry results have been written
    // accurately. batchSize must be 2 so the cursor recognizes exhaustion.
    assert.commandWorked(db.runCommand({
        getMore: cursor.getId(),
        collection: coll.getName(),
        batchSize: 2
    }));  // returns 1 doc, exhausts the cursor
    // The $telemetry query for the previous `getTelemetry` is included in this call to $telemetry.
    telemetry = getTelemetry(db);
    assert.eq(2, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 1,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 2,
                          /* expectedDocsReturnedSumOfSq */ 4);

    // Run more queries (to exhaustion) with the same query shape, and ensure telemetry results are
    // accurate.
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 5}}},
        {$project: {hello: "$world"}},
    ]);  // returns 2 docs
    coll.aggregate([
        {$match: {v: {$gt: 2, $lt: 3}}},
        {$project: {hello: "$universe"}},
    ]);  // returns 0 docs
    coll.aggregate([
        {$match: {v: {$gt: 0, $lt: 2}}},
        {$project: {hello: "$galaxy"}},
    ]);  // returns 1 doc
    telemetry = getTelemetry(db);
    assert.eq(2, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 4,
                          /* expectedDocsReturnedSum */ 5,
                          /* expectedDocsReturnedMax */ 2,
                          /* expectedDocsReturnedMin */ 0,
                          /* expectedDocsReturnedSumOfSq */ 9);

    st.stop();
}

// Assert on batchSize-limited find queries that killCursors will write metrics with partial results
// to the telemetry store.
{
    const st = setup();
    const db = st.s.getDB("test");
    const collName = "coll";
    const coll = db[collName];

    const telemetryKey = {
        queryShape: {
            cmdNs: {db: "test", coll: "coll"},
            find: collName,
            filter: {$and: [{v: {$gt: "?number"}}, {v: {$lt: "?number"}}]},
            batchSize: "?number",
        },
        readConcern: {level: "local", provenance: "implicitDefault"},
        applicationName: "MongoDB Shell"
    };

    const cursor1 = coll.find({v: {$gt: 0, $lt: 5}}).batchSize(1);  // returns 1 doc
    const cursor2 = coll.find({v: {$gt: 0, $lt: 2}}).batchSize(1);  // returns 1 doc

    assert.commandWorked(
        db.runCommand({killCursors: coll.getName(), cursors: [cursor1.getId(), cursor2.getId()]}));

    const telemetry = getTelemetry(db);
    assert.eq(1, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 2,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 2);
    st.stop();
}

// Assert on batchSize-limited agg queries that killCursors will write metrics with partial results
// to the telemetry store.
{
    const st = setup();
    const db = st.s.getDB("test");
    const coll = db.coll;

    const telemetryKey = {
        pipeline: [{$match: {v: {$gt: aggRedactString, $lt: aggRedactString}}}],
        namespace: `test.${coll.getName()}`,
        applicationName: "MongoDB Shell"
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

    const telemetry = getTelemetry(db);
    assert.eq(1, telemetry.length);
    assertExpectedResults(telemetry[0],
                          telemetryKey,
                          /* expectedExecCount */ 2,
                          /* expectedDocsReturnedSum */ 2,
                          /* expectedDocsReturnedMax */ 1,
                          /* expectedDocsReturnedMin */ 1,
                          /* expectedDocsReturnedSumOfSq */ 2);
    st.stop();
}
}());
