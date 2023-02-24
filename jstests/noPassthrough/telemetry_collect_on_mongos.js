/**
 * Test that mongos is collecting metrics.
 */

(function() {
"use strict";

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryConfigureTelemetrySamplingRate: 2147483647,
            featureFlagTelemetry: true,
        }
    },
});

// Redacted literal replacement string. This may change in the future, so it's factored out.
const R = "###";

const mongos = st.s;
const db = mongos.getDB("test");
const coll = db.coll;
coll.insert({v: 1});
coll.insert({v: 4});

// Get the telemetry for a given database, filtering out the actual $telemetry call.
const getTelemetry = (conn) => {
    const result = conn.adminCommand({
        aggregate: 1,
        pipeline: [
            {$telemetry: {}},
            // Sort on telemetry key so entries are in a deterministic order.
            {$sort: {key: 1}},
        ],
        cursor: {}
    });
    return result.cursor.firstBatch;
};

/**
 * Verify that mongos is recording these metrics:
 * - "firstSeenTimestamp"
 * - "lastExecutionMicros"
 * - "execCount"
 * - "queryExecMicros"
 * - "docsReturned"
 */

// This test can't predict exact timings, so just assert these three fields have been set (are
// non-zero).
const assertTelemetryMetricsSet = (metrics) => {
    const {firstSeenTimestamp, lastExecutionMicros, queryExecMicros} = metrics;

    assert.neq(timestampCmp(firstSeenTimestamp, Timestamp(0, 0)), 0);
    assert.neq(lastExecutionMicros, NumberLong(0));

    const distributionFields = ['sum', 'max', 'min', 'sumOfSquares'];
    for (const field of distributionFields) {
        assert.neq(queryExecMicros[field], NumberLong(0));
    }
};

coll.find({v: {$gt: 0, $lt: 5}}).toArray();
coll.find({v: {$gt: 2, $lt: 3}}).toArray();
coll.find({v: {$gt: 0, $lt: 1}}).toArray();
coll.find({v: {$gt: 0, $lt: 2}}).toArray();

{
    const telemetry = getTelemetry(db);
    assert.eq(1, telemetry.length);
    const {key, metrics} = telemetry[0];
    const {docsReturned, execCount} = metrics;
    assert.eq(4, execCount);
    assert.eq(
        {
            find: {
                find: R,
                filter: {v: {$gt: R, $lt: R}},
                readConcern: {level: R, provenance: R},
            },
            namespace: `test.${coll.getName()}`,
            readConcern: {level: "local", provenance: "implicitDefault"},
            applicationName: "MongoDB Shell"
        },
        key,
    );
    assert.eq({
        sum: NumberLong(3),
        max: NumberLong(2),
        min: NumberLong(0),
        sumOfSquares: NumberLong(5),
    },
              docsReturned);
    assertTelemetryMetricsSet(metrics);
}

coll.aggregate([
    {$match: {v: {$gt: 0, $lt: 5}}},
    {$project: {hello: "$world"}},
]);
coll.aggregate([
    {$match: {v: {$gt: 0, $lt: 5}}},
    {$project: {hello: "$world"}},
]);
coll.aggregate([
    {$match: {v: {$gt: 2, $lt: 3}}},
    {$project: {hello: "$universe"}},
]);
coll.aggregate([
    {$match: {v: {$gt: 0, $lt: 2}}},
    {$project: {hello: "$galaxy"}},
]);

{
    const telemetry = getTelemetry(mongos.getDB("test"));
    assert.eq(3, telemetry.length);  // The $telemetry query for the last test is included in this
                                     // call to $telemetry.
    const {key, metrics} = telemetry[1];
    const {docsReturned, execCount} = metrics;
    assert.eq(4, execCount);
    assert.eq({
        sum: NumberLong(5),
        max: NumberLong(2),
        min: NumberLong(0),
        sumOfSquares: NumberLong(9),
    },
              docsReturned);
    assert.eq({
        pipeline: [
            {$match: {v: {$gt: R, $lt: R}}},
            {$project: {hello: R}},
        ],
        namespace: "test.coll",
        applicationName: "MongoDB Shell"
    },
              key);
    assertTelemetryMetricsSet(metrics);
}

st.stop();
}());
