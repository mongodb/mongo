/**
 * Tests $out stage of aggregate command concurrently with killOp. Ensures that all the temporary
 * collections created during aggreate command are deleted. If extending this workload, consider
 * overriding the following:
 * - $config.states.aggregate: The function to execute the aggregation.
 * - $config.states.killOp: The function to find the aggregation and kill it. Consider reusing
 *   $config.data.killOpsMatchingFilter to do the deed.
 * - $config.teardown: If you want any assertion to make sure nothing got leaked or left behind by
 *   the interrupted aggregation.
 *
 * @tags: [
 *  uses_curop_agg_stage,
 *  # TODO Undenylist (SERVER-38852).
 *  assumes_against_mongod_not_mongos,
 *  requires_getmore,
 *  # The 'killOp' state uses the killOp cmd, which is incompatible with txns, and a getMore in
 *  # same state. As a result the getMore will be run outside of a txn.
 *  uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.aggregate = function aggregate(db, collName) {
        // $out to the same collection so that concurrent aggregate commands would cause congestion.
        db[collName].runCommand(
            {aggregate: collName, pipeline: [{$out: "interrupt_temp_out"}], cursor: {}});
    };

    // This test sets up aggregations just to tear them down. There's no benefit to using large
    // documents here, and doing so can increase memory pressure on the test host, so we lower it
    // down to 1KB.
    $config.data.docSize = 1024;
    $config.data.killOpsMatchingFilter = function killOpsMatchingFilter(db, filter) {
        const currentOpOutput =
            db.getSiblingDB('admin').aggregate([{$currentOp: {}}, {$match: filter}]).toArray();
        for (let op of currentOpOutput) {
            assert(op.hasOwnProperty('opid'));
            assert.commandWorked(db.getSiblingDB('admin').killOp(op.opid));
        }
    };
    $config.states.killOp = function killOp(db, collName) {
        // The aggregate command could be running different commands internally (renameCollection,
        // insertDocument, etc.) depending on which stage of execution it is in. So, get all the
        // operations that are running against the input, output or temp collections.
        const filter = {
            op: "command",
            active: true,
            $or: [
                {"ns": db.getName() + ".interrupt_temp_out"},              // For output collection.
                {"ns": db.getName() + "." + collName},                     // For input collection.
                {"ns": {$regex: "^" + db.getName() + "\.tmp\.agg_out.*"}}  // For temp during $out.
            ],
            "command.drop": {
                $exists: false
            }  // Exclude 'drop' command from the filter to make sure that we don't kill the the
               // drop command which is responsible for dropping the temporary collection.
        };
        if (TestData.testingReplicaSetEndpoint) {
            // The sharding DDL operations do not have opid.
            filter["$and"] = [
                {desc: {$ne: "CreateCollectionCoordinator"}},
                {desc: {$ne: "DropCollectionCoordinator"}},
                {desc: {$ne: "DropParticipantInstance"}},
                {desc: {$ne: "RenameCollectionCoordinator"}},
                {desc: {$ne: "RenameParticipantInstance"}},
            ];
        }
        this.killOpsMatchingFilter(db, filter);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Ensure that no temporary collection is left behind.
        if (TestData.testingReplicaSetEndpoint) {
            // When testing replica set endpoint, the temporary collection might not get deleted.
            // Instead, it will be cleaned up on the next step up.
            return;
        }
        assert.eq(db.getCollectionNames().filter(col => col.includes('tmp.agg_out')).length, 0);
    };

    $config.transitions = {
        aggregate: {aggregate: 0.8, killOp: 0.2},
        killOp: {aggregate: 0.8, killOp: 0.2}
    };

    $config.startState = 'aggregate';

    return $config;
});
