/**
 * Tests query stats for delete commands that go through various mongos dispatch paths.
 * @tags: [featureFlagQueryStatsDelete]
 */
import {getQueryStatsDeleteCmd} from "jstests/libs/query/query_stats_utils.js";
import {runMongosWriteMetricsTests} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const testCommands = {
    multiAll: (coll) => ({delete: coll, deletes: [{q: {}, limit: 0}]}),
    multiTargeted: (coll) => ({delete: coll, deletes: [{q: {_id: {$gt: 0}}, limit: 0}]}),
    singleOp: (coll, filterValue) => ({
        delete: coll,
        deletes: [{q: {filterField: filterValue}, limit: 1}],
    }),
    noMatch: (coll) => ({
        delete: coll,
        deletes: [{q: {filterField: "nonexistent"}, limit: 1}],
    }),
    batchOp: (coll, f1, f2) => ({
        delete: coll,
        deletes: [
            {q: {filterField: f1}, limit: 1},
            {q: {filterField: f2}, limit: 1},
        ],
    }),
    byId: (coll) => ({delete: coll, deletes: [{q: {_id: 1}, limit: 1}]}),
};

runMongosWriteMetricsTests({
    label: "delete",
    commands: testCommands,
    validateWriteMetricsFn: (n) => ({
        nMatched: 0,
        nUpserted: 0,
        nModified: 0,
        nDeleted: n,
        nInserted: 0,
        nUpdateOps: 0,
    }),
    validateCmdFn: (result) => assert.eq(result.n, 1, result),
    getQueryStatsFn: getQueryStatsDeleteCmd,
    // TODO SERVER-128278 remove special handling for deletes on sharded clusters.
    docsExaminedOverride: true,
});
