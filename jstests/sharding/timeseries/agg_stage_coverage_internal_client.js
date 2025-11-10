/**
 * Tests that aggregation stages that require internal clients work correctly on timeseries collections.
 * See 'agg_stage_coverage.js' for more details about this test and how to add to this file.
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

function makeInternalConn(conn) {
    const curDB = conn.getDB(dbName);
    assert.commandWorked(
        curDB.runCommand({
            ["hello"]: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return conn;
}

const dbName = jsTestName();
const st = new ShardingTest({shards: 2, mongos: 1});
const internalConn = makeInternalConn(st.rs0.getPrimary());
const internalDB = internalConn.getDB(dbName);
const tsColl = internalDB["timeseries"];
assertDropCollection(internalDB, tsColl.getName());
assert.commandWorked(
    internalDB.runCommand({
        create: tsColl.getName(),
        timeseries: {timeField: "time", metaField: "m"},
        writeConcern: {w: "majority"},
    }),
);
// Insert 10 documents, so the aggregation stages will return some documents to confirm the aggregation stage worked.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(
        tsColl.insert(
            {_id: i, time: new Date(), m: {tag: "A", loc: [40, 40]}, value: i * 10},
            {writeConcern: {w: "majority"}},
        ),
    );
}

const tests = [
    {stage: "$_internalDensify", pipeline: [{$_internalDensify: {field: "value", range: {step: 10, bounds: "full"}}}]},
    {
        stage: "$mergeCursors",
        pipeline: [
            {
                $mergeCursors: {
                    nss: tsColl.getName(),
                    sort: {m: 1},
                    remotes: [],
                    compareWholeSortKey: false,
                    allowPartialResults: false,
                },
            },
        ],
        // $mergeCursors is only ever made right before execution when the view is already resolved and therefore will
        // work with viewful timeseries. So there is no bug in production, since we would never run $mergeCursors on
        // the timeseries view, but this test case will fail.
        skipTest: !areViewlessTimeseriesEnabled(internalDB),
        zeroDocsReturned: true,
    },
    {stage: "$setMetadata", pipeline: [{$setMetadata: {score: "$value"}}]},
];

tests.forEach((test) => {
    if (test.skipTest) {
        jsTest.log.info("Skipping " + test.stage + " test on timeseries collections.");
        return;
    }
    const cursor = internalDB.runCommand({
        aggregate: tsColl.getName(),
        pipeline: test.pipeline,
        cursor: {},
        writeConcern: {w: "majority"},
        readConcern: {level: "snapshot"},
    });
    assert.commandWorked(cursor, test.stage + " failed on timeseries collections.");

    const result = cursor.cursor.firstBatch;
    if (test.zeroDocsReturned) {
        assert.eq(result.length, 0, test.stage + " expected to return zero documents on timeseries collections.");
        return;
    }
    assert(
        result.length > 0,
        test.stage + " expected to return documents on timeseries collections. Received: " + tojson(cursor),
    );
    // Confirm the documents were not bucket documents. We will just look at the first document
    // and ensure there is no "control.min.time" field which all bucket documents have.
    assert(
        !result[0].hasOwnProperty("control"),
        test.stage + " expected to not return bucket documents on timeseries collections. Received: " + tojson(cursor),
    );
});

st.stop();
