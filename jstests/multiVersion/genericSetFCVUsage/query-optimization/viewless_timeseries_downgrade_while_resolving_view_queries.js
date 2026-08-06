/**
 * Tests that a query whose namespace - or whose foreign namespace - is a user view over a viewless
 * timeseries collection returns correct results when an FCV downgrade converts that collection back
 * to the viewful format mid-query.
 *
 * Covers 4 different mechanisms by which a subpipeline stage's resolution reaches the shard, each
 * of which composes the second resolution differently: $graphLookup, $lookup, $unionWith and
 * $facet.Then covers count, distinct and find directly against the view, each over both an untracked
 * and a sharded timeseries collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO(SERVER-111172): Remove this test once 9.0 becomes lastLTS.
if (lastLTSFCV != "8.0") {
    quit();
}

const st = new ShardingTest({shards: 1});

if (!areViewlessTimeseriesEnabled(st.s.getDB("admin"))) {
    jsTest.log.info("Skipping test: viewless timeseries collections are not enabled");
    st.stop();
    quit();
}

const normalCollName = "normal";
const tsCollName = "ts";
const viewName = "ts_view";

const shardPrimary = st.rs0.getPrimary();
const shardAdmin = shardPrimary.getDB("admin");

function setFCV(version) {
    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: version, confirm: true}),
    );
}

function setUpCase(caseDbName, shardTsCollection) {
    // Create a plain collection, a viewless timeseries, and a user view over the timeseries
    // collection. Must be called under latestFCV so that 'ts' is created viewless.
    const caseDB = st.s.getDB(caseDbName);
    assert.commandWorked(caseDB.dropDatabase());

    assert.commandWorked(
        caseDB[normalCollName].insertMany([
            {_id: 1, key: "active"},
            {_id: 2, key: "inactive"},
        ]),
    );

    assert.commandWorked(caseDB.createCollection(tsCollName, {timeseries: {timeField: "time"}}));
    assert.commandWorked(
        caseDB[tsCollName].insertMany([
            {_id: 1, status: "active", time: ISODate("2025-09-01T13:00:00Z"), age: 25},
            {_id: 2, status: "inactive", time: ISODate("2025-09-01T14:00:00Z"), age: 30},
            // Excluded by the view's predicate.
            {_id: 3, status: "active", time: ISODate("2025-09-01T15:00:00Z"), age: 35},
        ]),
    );

    if (shardTsCollection) {
        assert.commandWorked(st.s.adminCommand({enableSharding: caseDbName}));
        assert.commandWorked(caseDB[tsCollName].createIndex({time: 1}));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: caseDbName + "." + tsCollName, key: {time: 1}}),
        );
    }

    assert.commandWorked(caseDB.createView(viewName, tsCollName, [{$match: {age: {$lte: 30}}}]));
    return caseDB;
}

function aggCmd(pipeline) {
    return {aggregate: normalCollName, pipeline: pipeline, cursor: {}};
}

/**
 * Extracts the comparable results out of a command reply. 'resultKind' names where each command
 * puts them: a cursor ('aggregate', 'find'), a number ('n'), or an array of values ('distinct').
 *
 * The same switch is duplicated inside the Thread below, which cannot see this scope.
 */
function extractResults(db, res, resultKind) {
    switch (resultKind) {
        case "cursor":
            return new DBCommandCursor(db, res).toArray();
        case "n":
            return [{n: res.n}];
        case "values":
            return [{values: res.values.slice().sort((a, b) => (a < b ? -1 : a > b ? 1 : 0))}];
        default:
            throw new Error("unknown resultKind: " + resultKind);
    }
}

/**
 * Runs 'command' while an FCV downgrade converts the timeseries collection to the viewful format
 * underneath it, and asserts the results are 'expected'.
 */
function runWithDowngradeMidQuery({
    caseDbName,
    command,
    resultKind,
    expected,
    extraErrorMsg,
    failPointCollName = normalCollName,
    skipHits = 1,
    shardTsCollection = false,
}) {
    // Must reset the FCV to latest so the timeseries collection is made viewless.
    setFCV(latestFCV);
    const caseDB = setUpCase(caseDbName, shardTsCollection);

    // The query must already be correct with no FCV change in flight.
    assert.sameMembers(
        expected,
        extractResults(caseDB, assert.commandWorked(caseDB.runCommand(command)), resultKind),
        "baseline query is wrong",
    );

    // Raise query verbosity so that the "found view definition" logs are emitted if a stage
    // resolves a view at execution time.
    shardAdmin.setLogLevel(3, "query");

    const fp = configureFailPoint(
        shardPrimary,
        "hangAfterAcquiringCollectionCatalog",
        {collection: failPointCollName},
        {skip: skipHits},
    );

    // Runs the query and hands the results back to the main shell, which does the comparison.
    const aggThread = new Thread(
        function (host, dbName, command, resultKind) {
            const conn = new Mongo(host);
            const db = conn.getDB(dbName);

            const res = assert.commandWorked(db.runCommand(command));

            // Kept in sync with 'extractResults()' in the enclosing file, which a Thread cannot
            // see.
            switch (resultKind) {
                case "cursor":
                    return new DBCommandCursor(db, res).toArray();
                case "n":
                    return [{n: res.n}];
                case "values":
                    return [
                        {values: res.values.slice().sort((a, b) => (a < b ? -1 : a > b ? 1 : 0))},
                    ];
                default:
                    throw new Error("unknown resultKind: " + resultKind);
            }
        },
        st.s.host,
        caseDbName,
        command,
        resultKind,
    );

    aggThread.start();
    try {
        fp.wait();

        // Downgrade FCV while aggregation is paused - converts viewless to viewful.
        setFCV(lastLTSFCV);
    } finally {
        // Release the query and reset the settings even if the downgrade failed, so that a failure
        // here does not leave the failpoint, the raised log level or the thread behind for the
        // remaining test cases.
        fp.off();
        aggThread.join();
        shardAdmin.setLogLevel(0, "query");
    }

    assertArrayEq({actual: aggThread.returnData(), expected: expected, extraErrorMsg});

    // The same query must also be correct once the downgrade has fully settled.
    assert.sameMembers(
        expected,
        extractResults(caseDB, assert.commandWorked(caseDB.runCommand(command)), resultKind),
        "query is wrong after the downgrade completed",
    );
}

describe("view over a timeseries collection downgraded mid-query", function () {
    after(function () {
        setFCV(latestFCV);
        st.stop();
    });

    it("$graphLookup", function () {
        runWithDowngradeMidQuery({
            caseDbName: "graph_lookup",
            resultKind: "cursor",
            command: aggCmd([
                {
                    $graphLookup: {
                        from: viewName,
                        startWith: "$key",
                        connectFromField: "key",
                        connectToField: "status",
                        as: "matched",
                        maxDepth: 0,
                    },
                },
                {$project: {_id: 0, "matched.time": 0}},
            ]),
            expected: [
                {key: "active", matched: [{_id: 1, status: "active", age: 25}]},
                {key: "inactive", matched: [{_id: 2, status: "inactive", age: 30}]},
            ],
            extraErrorMsg:
                "the user view's predicate was dropped from '$_internalFromPipeline' when the" +
                " timeseries collection was concurrently converted to the viewful format",
        });
    });

    it("$lookup", function () {
        runWithDowngradeMidQuery({
            caseDbName: "lookup",
            resultKind: "cursor",
            command: aggCmd([
                {
                    $lookup: {
                        from: viewName,
                        localField: "key",
                        foreignField: "status",
                        as: "matched",
                    },
                },
                {$project: {_id: 0, "matched.time": 0}},
            ]),
            expected: [
                {key: "active", matched: [{_id: 1, status: "active", age: 25}]},
                {key: "inactive", matched: [{_id: 2, status: "inactive", age: 30}]},
            ],
            extraErrorMsg:
                "the user view's predicate was dropped, or the join $match was left at a stale" +
                " '$_internalFieldMatchPipelineIdx', when the timeseries collection was" +
                " concurrently converted to the viewful format",
        });
    });

    it("$unionWith", function () {
        // The shorthand form (no user subpipeline).
        runWithDowngradeMidQuery({
            caseDbName: "union_with",
            resultKind: "cursor",
            command: aggCmd([{$unionWith: viewName}, {$project: {_id: 0, time: 0}}]),
            expected: [
                {key: "active"},
                {key: "inactive"},
                {status: "active", age: 25},
                {status: "inactive", age: 30},
            ],
            extraErrorMsg:
                "the user view's predicate was dropped from the materialized $unionWith" +
                " subpipeline when the timeseries collection was concurrently converted to the" +
                " viewful format",
        });
    });

    it("$facet", function () {
        // $facet reparses its subpipelines from raw BSON.
        runWithDowngradeMidQuery({
            caseDbName: "facet",
            resultKind: "cursor",
            command: aggCmd([
                {
                    $facet: {
                        viewCount: [
                            {$match: {_id: {$exists: false}}},
                            {$unionWith: viewName},
                            {$count: "n"},
                        ],
                    },
                },
            ]),
            expected: [{viewCount: [{n: 2}]}],
            extraErrorMsg:
                "the user view's predicate was dropped from the $unionWith nested inside $facet" +
                " when the timeseries collection was concurrently converted to the viewful format",
        });
    });

    // The remaining cases run directly against the view. Each runs over an untracked timeseries
    // collection, where the primary shard does all of the execution, and over a sharded collection
    // where the router handles retrying the operation.
    for (const shardTsCollection of [false, true]) {
        const topology = shardTsCollection ? "sharded" : "untracked";
        const describedTopology = shardTsCollection ? "a sharded" : "an untracked";
        const retriedBy = shardTsCollection ? "mongos" : "the shard";
        const viewCommandCaseDefaults = {
            failPointCollName: tsCollName,
            skipHits: 0,
            shardTsCollection,
        };

        it(`count on the view over ${describedTopology} timeseries collection`, function () {
            runWithDowngradeMidQuery({
                ...viewCommandCaseDefaults,
                caseDbName: `count_${topology}`,
                resultKind: "n",
                command: {count: viewName},
                // Only the two measurements the view admits, not the third.
                expected: [{n: 2}],
                extraErrorMsg:
                    "the count returned a tally that does not match the view's predicate after" +
                    " the timeseries collection was concurrently converted to the viewful format;" +
                    ` ${retriedBy} owns the retry in this topology`,
            });
        });

        it(`distinct on the view over ${describedTopology} timeseries collection`, function () {
            runWithDowngradeMidQuery({
                ...viewCommandCaseDefaults,
                caseDbName: `distinct_${topology}`,
                resultKind: "values",
                // 'age' rather than 'status': the excluded measurement shares its status with an
                // admitted one, so only 'age' makes a leak visible.
                command: {distinct: viewName, key: "age"},
                expected: [{values: [25, 30]}],
                extraErrorMsg:
                    "the distinct returned values outside the view's predicate after the" +
                    " timeseries collection was concurrently converted to the viewful format;" +
                    ` ${retriedBy} owns the retry in this topology`,
            });
        });

        it(`find on the view over ${describedTopology} timeseries collection`, function () {
            runWithDowngradeMidQuery({
                ...viewCommandCaseDefaults,
                caseDbName: `find_${topology}`,
                resultKind: "cursor",
                command: {find: viewName, filter: {}, projection: {_id: 1, status: 1, age: 1}},
                expected: [
                    {_id: 1, status: "active", age: 25},
                    {_id: 2, status: "inactive", age: 30},
                ],
                extraErrorMsg:
                    "the find returned documents outside the view's predicate after the" +
                    " timeseries collection was concurrently converted to the viewful format;" +
                    ` ${retriedBy} owns the retry in this topology`,
            });
        });
    }
});
