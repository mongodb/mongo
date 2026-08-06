/**
 * Tests that aggregate, count, distinct and find - and their explains - never leak the
 * 'IFRFlagRetry' kickback to the client when they run against a view whose pipeline joins a viewful
 * timeseries collection and directly on a viewful timeseries collection.
 *
 * Under lastLTSFCV a timeseries collection is a view over its buckets collection. Resolving such a
 * namespace as an involved namespace of an aggregation raises an 'IFRFlagRetry' kickback so that the
 * aggregation is retried with 'featureFlagExtensionsInsideHybridSearch' disabled.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// TODO(SERVER-111172): Remove this test once viewful timeseries collections are gone.
if (lastLTSFCV != "8.0") {
    quit();
}

const st = new ShardingTest({shards: 3});

const normalCollName = "normal";
const tsCollName = "ts";
const joinViewName = "join_view";

const untrackedDbName = "untracked_view_source";
const shardedDbName = "sharded_view_source";
const shardedTsDbName = "sharded_view_source_and_ts";

function setFCV(version) {
    assert.commandWorked(
        st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: version, confirm: true}),
    );
}

// Number of times the shards raised the timeseries-view IFR flag kickback.
function kickbackRetryCount() {
    let numKickbacks = 0;
    st.getAllShards().forEach((rs) => {
        const metrics = assert.commandWorked(
            rs.getPrimary().adminCommand({serverStatus: 1}),
        ).metrics;
        numKickbacks += metrics.query.extensionsInsideHybridSearch.timeseriesViewKickbackRetries;
    });

    return numKickbacks;
}

function setUpCase(caseDbName, {shardViewSource, shardTsCollection}) {
    // Must be called under lastLTSFCV so that 'ts' is created in the viewful format.
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

    if (shardViewSource || shardTsCollection) {
        assert.commandWorked(st.s.adminCommand({enableSharding: caseDbName}));
    }

    if (shardViewSource) {
        // Sharding the collection the view is defined over.
        assert.commandWorked(
            st.s.adminCommand({shardCollection: `${caseDbName}.${normalCollName}`, key: {_id: 1}}),
        );
    }

    if (shardTsCollection) {
        // Sharding the timeseries collection the view joins. Because a shard cannot read a sharded
        // foreign collection out of its own data, this forces the '$unionWith' onto the merging node
        // of the pipeline rather than leaving it on the shard that owns the view's source.
        //
        // The collection already holds measurements, so 'shardCollection' needs a supporting index.
        assert.commandWorked(caseDB[tsCollName].createIndex({time: 1}));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: `${caseDbName}.${tsCollName}`, key: {time: 1}}),
        );
    }

    // A view over a plain collection that joins the timeseries collection.
    assert.commandWorked(
        caseDB.createView(joinViewName, normalCollName, [
            {$unionWith: {coll: tsCollName, pipeline: [{$match: {age: {$lte: 30}}}]}},
        ]),
    );
}

/**
 * Extracts the comparable results out of a command reply. 'resultKind' names where each command puts
 * them: a cursor ('find'), a number ('n'), or an array of values ('distinct').
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

function runCommandAndValidateResults(caseDB, command, resultKind, expected) {
    const res = assert.commandWorked(
        caseDB.runCommand(command),
        `the IFR flag kickback raised while resolving the timeseries collection as an involved` +
            ` namespace was not absorbed and retried.`,
        {command},
    );

    if (resultKind) {
        assertArrayEq({
            actual: extractResults(caseDB, res, resultKind),
            expected: expected,
            extraErrorMsg: `the results did not survive the IFR flag retry performed.`,
        });
    }
}

function runAndExpectAbsorbedKickback({caseDbName, command, resultKind, expected}) {
    const caseDB = st.s.getDB(caseDbName);
    const countBefore = kickbackRetryCount();
    runCommandAndValidateResults(caseDB, command, resultKind, expected);

    assert.gt(
        kickbackRetryCount(),
        countBefore,
        "no IFR flag kickback was raised, so this case did not exercise the retry at all",
        {command},
    );
}

function runAndExpectResultsAndNoKickback({caseDbName, command, resultKind, expected}) {
    const caseDB = st.s.getDB(caseDbName);
    const countBefore = kickbackRetryCount();
    runCommandAndValidateResults(caseDB, command, resultKind, expected);

    assert.eq(
        kickbackRetryCount(),
        countBefore,
        "no IFR flag kickback was raised, so this case did not exercise the retry at all",
        {command},
    );
}

// The placements the view's source collection and the joined timeseries collection are tested in.
// 'desc' completes the phrase "... on a view over ___".
const topologies = [
    {
        desc: "an untracked collection",
        dbName: untrackedDbName,
        shardViewSource: false,
        shardTsCollection: false,
    },
    {
        desc: "a sharded collection",
        dbName: shardedDbName,
        shardViewSource: true,
        shardTsCollection: false,
    },
    {
        desc: "a sharded collection joining a sharded timeseries collection",
        dbName: shardedTsDbName,
        shardViewSource: true,
        shardTsCollection: true,
    },
];

describe("command against a viewful timeseries collection", function () {
    before(function () {
        // Viewful timeseries collections are what make the involved-namespace resolution raise the
        // kickback, so everything is created and queried under lastLTSFCV.
        setFCV(lastLTSFCV);
        topologies.forEach((topology) => setUpCase(topology.dbName, topology));
    });

    after(function () {
        setFCV(latestFCV);
        st.stop();
    });

    for (const {desc: describedTopology, dbName: caseDbName} of topologies) {
        it(`count on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {count: joinViewName},
                resultKind: "n",
                // The two plain documents plus the two measurements the union admits.
                expected: [{n: 4}],
            });
        });

        it(`explain count on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {explain: {count: joinViewName}},
            });
        });

        it(`distinct on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                // Only the measurements have an 'age', so the plain documents contribute nothing.
                command: {distinct: joinViewName, key: "age"},
                resultKind: "values",
                expected: [{values: [25, 30]}],
            });
        });

        it(`explain distinct on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {explain: {distinct: joinViewName, key: "age"}},
            });
        });

        it(`find on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {
                    find: joinViewName,
                    filter: {},
                    projection: {_id: 0, key: 1, status: 1, age: 1},
                },
                resultKind: "cursor",
                expected: [
                    {key: "active"},
                    {key: "inactive"},
                    {status: "active", age: 25},
                    {status: "inactive", age: 30},
                ],
            });
        });

        it(`explain find on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {
                    explain: {
                        find: joinViewName,
                        filter: {},
                        projection: {_id: 0, key: 1, status: 1, age: 1},
                    },
                },
            });
        });

        // Unlike count/distinct/find, an aggregation is not derived from another command, so the
        // kickback is absorbed by the aggregation's own retry loop rather than by a wrapper in the
        // calling command.
        it(`aggregate on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {
                    aggregate: joinViewName,
                    pipeline: [{$project: {_id: 0, time: 0}}],
                    cursor: {},
                },
                resultKind: "cursor",
                expected: [
                    {key: "active"},
                    {key: "inactive"},
                    {status: "active", age: 25},
                    {status: "inactive", age: 30},
                ],
            });
        });

        // Here the view is an involved namespace of the user's own pipeline rather than the
        // aggregation's main namespace.
        it(`aggregate joining a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {
                    aggregate: normalCollName,
                    pipeline: [{$unionWith: joinViewName}, {$count: "n"}],
                    cursor: {},
                },
                resultKind: "cursor",
                // The two plain documents, plus the view's four (its own two plus the two measurements
                // the union admits).
                expected: [{n: 6}],
            });
        });

        it(`explain aggregate on a view over ${describedTopology}`, function () {
            runAndExpectAbsorbedKickback({
                caseDbName,
                command: {
                    explain: {aggregate: joinViewName, pipeline: [], cursor: {}},
                },
            });
        });

        // The same commands run directly on the timeseries collection. No kickback is expected: the
        // timeseries collection is the main namespace here, not an involved one. The view's
        // predicate does not apply either, so every measurement is returned.
        it(`count on the timeseries collection in '${caseDbName}'`, function () {
            runAndExpectResultsAndNoKickback({
                caseDbName,
                command: {count: tsCollName},
                resultKind: "n",
                expected: [{n: 3}],
            });
        });

        it(`distinct on the timeseries collection in '${caseDbName}'`, function () {
            runAndExpectResultsAndNoKickback({
                caseDbName,
                command: {distinct: tsCollName, key: "age"},
                resultKind: "values",
                expected: [{values: [25, 30, 35]}],
            });
        });

        it(`find on the timeseries collection in '${caseDbName}'`, function () {
            runAndExpectResultsAndNoKickback({
                caseDbName,
                command: {
                    find: tsCollName,
                    filter: {},
                    projection: {_id: 0, status: 1, age: 1},
                },
                resultKind: "cursor",
                expected: [
                    {status: "active", age: 25},
                    {status: "inactive", age: 30},
                    {status: "active", age: 35},
                ],
            });
        });

        it(`aggregate on the timeseries collection in '${caseDbName}'`, function () {
            runAndExpectResultsAndNoKickback({
                caseDbName,
                command: {
                    aggregate: tsCollName,
                    pipeline: [{$project: {_id: 0, time: 0}}],
                    cursor: {},
                },
                resultKind: "cursor",
                expected: [
                    {status: "active", age: 25},
                    {status: "inactive", age: 30},
                    {status: "active", age: 35},
                ],
            });
        });
    }
});
