/**
 * Runs a variety commands which need to interact with the global cursor manager. This test was
 * designed to reproduce SERVER-33959.
 *
 * The "grandparent test," invalidated_cursors.js, uses $currentOp.
 * @tags: [
 *   uses_curop_agg_stage,
 *   state_functions_share_cursor,
 *   assumes_balancer_off,
 *   requires_getmore
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/kill_multicollection_aggregation.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.listCollections = function listCollections(unusedDB, _) {
        const db = unusedDB.getSiblingDB(this.uniqueDBName);
        const cmdRes = db.runCommand({listCollections: 1, cursor: {batchSize: $config.data.batchSize}});
        assert.commandWorked(cmdRes);
        this.cursor = new DBCommandCursor(db, cmdRes);
    };

    $config.states.listIndexes = function listIndexes(unusedDB, _) {
        const db = unusedDB.getSiblingDB(this.uniqueDBName);
        const targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
        const cmdRes = db.runCommand({listIndexes: targetCollName, cursor: {batchSize: $config.data.batchSize}});
        // We expect this might fail if the namespace does not exist, otherwise it should always
        // succeed.
        if (cmdRes.code != ErrorCodes.NamespaceNotFound) {
            assert.commandWorked(cmdRes);
            this.cursor = new DBCommandCursor(db, cmdRes);
        }
    };

    // 'extendWorkload' will have copied the transitions already, here we modify the transitions to
    // include our additional states. This will make it so the total probability is not equal to
    // one, but this is allowed, and the probabilities will be scaled. We aren't overly concerned
    // with the relative probability of the events, so long as these new states happen relatively
    // frequently.
    $config.transitions.listCollections = {killCursor: 0.1, getMore: 0.9};
    $config.transitions.listIndexes = {killCursor: 0.1, getMore: 0.9};
    $config.transitions.init.listCollections = 0.25;
    $config.transitions.init.listIndexes = 0.25;

    return $config;
});
