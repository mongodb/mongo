/**
 * Regression test for BF-41937.
 *
 * Reproduces a race where movePrimary relocates system.views to a different shard while a
 * $lookup on a timeseries view is in flight. The recipient shard resolves the view but produces
 * a ResolvedView with timeseriesUsesExtendedRange=false (since the flag is per-node/in-memory
 * and that shard never received extended range inserts). This ResolvedView is sent back via
 * CommandOnShardedViewNotSupportedOnMongod to the executing shard, which rebuilds the
 * sub-pipeline with the incorrect flag, enabling BoundedSorter on out-of-range dates.
 *
 * The failpoint hangBeforeCreatingAggCatalogState widens the naturally tiny window between
 * command dispatch and catalog state creation so movePrimary can complete in between.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_timeseries,
 *   assumes_balancer_off,
 *   requires_fcv_83,
 * ]
 */
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});
const dbName = jsTestName();
const testDB = st.getDB(dbName);

if (areViewlessTimeseriesEnabled(testDB)) {
    jsTestLog("Skipping: viewless timeseries bypasses the view-resolution path.");
    st.stop();
    quit();
}

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const foreignCollName = "foreign_coll";
const foreignViewName = "foreign_view";
const localCollName = "local_coll";

// Sharded timeseries collection with extended-range data, all on shard0.
assert.commandWorked(
    testDB.adminCommand({
        shardCollection: `${dbName}.${foreignCollName}`,
        key: {t: 1},
        timeseries: {timeField: "t"},
    }),
);
const allDocs = [
    {_id: 0, t: ISODate("1975-12-01")},
    {_id: 1, t: ISODate("1980-01-13")},
    {_id: 2, t: ISODate("2018-07-14")},
    {_id: 3, t: ISODate("2030-09-30")},
    {_id: 4, t: ISODate("1969-12-31T23:00:59.001Z")}, // pre-epoch
    {_id: 5, t: ISODate("1969-12-31T23:59:59.001Z")}, // pre-epoch
    {_id: 6, t: ISODate("2038-01-19T03:14:07.001Z")}, // post-2038
    {_id: 7, t: ISODate("2050-01-20T03:14:00.003Z")}, // post-2038
];
assert.commandWorked(testDB[foreignCollName].insert(allDocs));

// local_coll must be sharded so it stays on shard0 during movePrimary.
const localColl = testDB[localCollName];
assert.commandWorked(localColl.createIndex({a: 1}));
assert.commandWorked(testDB.adminCommand({shardCollection: `${dbName}.${localCollName}`, key: {a: 1}}));
assert.commandWorked(localColl.insert({_id: 0, a: 1}));

assert.commandWorked(testDB.createView(foreignViewName, foreignCollName, [{$set: {viewVal: 100}}]));

// Pause shard0 before it resolves namespaces / creates the ExpressionContext.
// Filter by namespace so FTDC and other internal aggregates are not affected.
const fp = configureFailPoint(st.shard0, "hangBeforeCreatingAggCatalogState", {ns: `${dbName}.${localCollName}`});

const awaitAggregate = startParallelShell(
    funWithArgs(
        function (dbName, localCollName, foreignViewName) {
            const res = db.getSiblingDB(dbName).runCommand({
                aggregate: localCollName,
                pipeline: [
                    {
                        $lookup: {
                            from: foreignViewName,
                            pipeline: [{$sort: {t: 1}}],
                            as: "result",
                        },
                    },
                ],
                cursor: {},
            });
            assert.commandWorked(res);
        },
        dbName,
        localCollName,
        foreignViewName,
    ),
    st.s.port,
);

fp.wait();

// Move system.views away from shard0 while the aggregate is paused.
assert.commandWorked(testDB.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

fp.off();
awaitAggregate();

st.stop();
