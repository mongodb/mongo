/**
 * Tests that hybrid index builds on timeseries collections behave correctly when they
 * receive concurrent writes.
 */
import {assertCommandWorkedInParallelShell} from "jstests/libs/parallel_shell_helpers.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const collName = "ts";
const testDB = conn.getDB(dbName);
const tsColl = testDB[collName];

const timeField = "time";
const metaField = "m";

const runTest = (config) => {
    // Populate the collection.
    tsColl.drop();

    assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    let nDocs = 10;
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(
            tsColl.insert({
                _id: i,
                [timeField]: new Date(),
            }),
        );
    }

    jsTestLog("Testing: " + tojson(config));

    // Prevent the index build from completing.
    IndexBuildTest.pauseIndexBuilds(conn);

    // Build a raw index over the buckets of the time-series collection, and wait for it to start.
    const indexName = "test_index";
    let indexOptions = config.indexOptions || {};
    indexOptions.name = indexName;
    const awaitIndex = assertCommandWorkedInParallelShell(conn, testDB, {
        createIndexes: getTimeseriesCollForRawOps(testDB, tsColl).getName(),
        indexes: [{key: config.indexSpec, ...indexOptions}],
        ...getRawOperationSpec(testDB),
    });
    IndexBuildTest.waitForIndexBuildToStart(testDB, tsColl.getName(), indexName);

    // Perform writes while the index build is in progress.
    assert.commandWorked(
        tsColl.insert({
            _id: nDocs++,
            [timeField]: new Date(),
        }),
    );

    let extraDocs = config.extraDocs || [];
    extraDocs.forEach((doc) => {
        const template = {_id: nDocs++, [timeField]: new Date()};
        const newObj = Object.assign({}, doc, template);
        assert.commandWorked(tsColl.insert(newObj));
    });

    // Allow the index build to complete.
    IndexBuildTest.resumeIndexBuilds(conn);
    awaitIndex();

    // Due to the nature of bucketing, we can't reliably make assertions about them,
    // so we rely on validate to ensure the index is built correctly.
    const validateRes = assert.commandWorked(tsColl.validate());
    assert(validateRes.valid, validateRes);
};

const basicOps = [
    {[metaField]: -Math.pow(-2147483648, 34)}, // -Inf
    {[metaField]: 0},
    {[metaField]: 0},
    {[metaField]: {foo: "bar"}},
    {[metaField]: {foo: 1}},
    {[metaField]: "hello world"},
    {[metaField]: 1},
    {},
    {[metaField]: Math.pow(-2147483648, 34)}, // Inf
];

runTest({
    indexSpec: {[metaField]: 1},
    extraDocs: basicOps,
});

runTest({
    indexSpec: {[metaField]: -1},
    extraDocs: basicOps,
});

runTest({
    indexSpec: {[metaField]: "hashed"},
    extraDocs: basicOps,
});

runTest({
    indexSpec: {[metaField + ".$**"]: 1},
    extraDocs: basicOps,
});
runTest({
    indexSpec: {[metaField + "$**"]: 1},
    extraDocs: basicOps,
});

runTest({
    indexSpec: {[metaField]: "2dsphere"},
    extraDocs: [
        {
            [metaField]: {
                type: "Polygon",
                coordinates: [
                    [
                        [0, 0],
                        [0, 1],
                        [1, 1],
                        [1, 0],
                        [0, 0],
                    ],
                ],
            },
        },
        {[metaField]: {type: "Point", coordinates: [0, 1]}},
    ],
});

runTest({
    indexSpec: {[metaField]: "2dsphere"},
    indexOptions: {sparse: true},
    extraDocs: [
        {
            [metaField]: {
                type: "Polygon",
                coordinates: [
                    [
                        [0, 0],
                        [0, 1],
                        [1, 1],
                        [1, 0],
                        [0, 0],
                    ],
                ],
            },
        },
        {[metaField]: {type: "Point", coordinates: [0, 1]}},
        {},
    ],
});

runTest({
    indexSpec: {"control.min.time": 1},
    extraDocs: basicOps,
});

runTest({
    indexSpec: {"control.max.time": -1},
    extraDocs: basicOps,
});

MongoRunner.stopMongod(conn);
