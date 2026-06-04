/**
 * Tests that timeseries resharding ops abort take provenance into account. Uses a non-"meta"
 * metaField name to exercise the shard key translation path
 * (user-facing field -> internal bucket field).
 *
 * Timeseries variant of resharding_abort_provenance_match.js.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagMoveCollection,
 *   featureFlagUnshardCollection,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getReshardingCoordinatorMetrics} from "jstests/sharding/libs/reshard_collection_util.js";

let st = new ShardingTest({mongos: 1, shards: 1});

const dbName = "db";
const collName = "foo";
const ns = dbName + "." + collName;
let mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    mongos.getDB(dbName).createCollection(collName, {
        timeseries: {timeField: "ts", metaField: "metaTest"},
    }),
);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {"metaTest.x": 1}}));

let failpoint = configureFailPoint(st.rs0.getPrimary(), "reshardingPauseRecipientDuringCloning");

const awaitResult = startParallelShell(
    funWithArgs(function (ns) {
        assert.commandFailedWithCode(
            db.adminCommand({reshardCollection: ns, key: {"metaTest.y": 1}, numInitialChunks: 1}),
            ErrorCodes.ReshardCollectionAborted,
        );
    }, ns),
    st.s.port,
);

failpoint.wait();

// In legacy timeseries (FCV < 9.0), resharding tracks the bucket namespace in $currentOp.
// In viewless timeseries (FCV >= 9.0), it tracks the user-facing namespace.
const reshardCollectionNs = getTimeseriesCollForDDLOps(
    mongos.getDB(dbName),
    mongos.getDB(dbName).getCollection(collName),
).getFullName();
const filter = {
    type: "op",
    "originatingCommand.reshardCollection": reshardCollectionNs,
    "provenance": "reshardCollection",
};
assert.soon(() => {
    return (
        st.s
            .getDB("admin")
            .aggregate([{$currentOp: {allUsers: true, localOps: false}}, {$match: filter}])
            .toArray().length >= 1
    );
});

assert.commandFailedWithCode(
    mongos.adminCommand({abortUnshardCollection: reshardCollectionNs}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    mongos.adminCommand({abortMoveCollection: reshardCollectionNs}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    mongos.adminCommand({abortRewriteCollection: reshardCollectionNs}),
    ErrorCodes.IllegalOperation,
);

assert.commandWorked(mongos.adminCommand({abortReshardCollection: reshardCollectionNs}));

failpoint.off();
awaitResult();

const metrics = getReshardingCoordinatorMetrics(st.configRS, "resharding");

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSucceeded, 0);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 1);

st.stop();
