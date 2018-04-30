'use strict';

/**
 * Runs a variety commands which need to interact with the global cursor manager. This test was
 * designed to reproduce SERVER-33959.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/kill_multicollection_aggregation.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.states.listCollections = function listCollections(unusedDB, _) {
        const db = unusedDB.getSiblingDB(this.uniqueDBName);
        const cmdRes =
            db.runCommand({listCollections: 1, cursor: {batchSize: $config.data.batchSize}});
        assertAlways.commandWorked(cmdRes);
        this.cursor = new DBCommandCursor(db, cmdRes);
    };

    $config.states.listIndexes = function listIndexes(unusedDB, _) {
        const db = unusedDB.getSiblingDB(this.uniqueDBName);
        const targetCollName = this.chooseRandomlyFrom(this.involvedCollections);
        const cmdRes = db.runCommand(
            {listIndexes: targetCollName, cursor: {batchSize: $config.data.batchSize}});
        // We expect this might fail if the namespace does not exist, otherwise it should always
        // succeed.
        if (cmdRes.code != ErrorCodes.NamespaceNotFound) {
            assertAlways.commandWorked(cmdRes);
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
