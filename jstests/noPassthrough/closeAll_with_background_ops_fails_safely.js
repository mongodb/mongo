/**
 * SERVER-35671: Ensure that if database has background operations it can't be closed and that
 * attempting to close it won't leave it in an inconsistant state.
 *
 * @tags: [requires_replication, uses_transactions]
 */

(function() {
    "use strict";

    load('jstests/noPassthrough/libs/index_build.js');

    let replSet = new ReplSetTest({name: "server35671", nodes: 1});
    let setFailpointBool = (failpointName, alwaysOn, times) => {
        if (times) {
            return db.adminCommand({configureFailPoint: failpointName, mode: {"times": times}});
        } else if (alwaysOn) {
            return db.adminCommand({configureFailPoint: failpointName, mode: "alwaysOn"});
        } else {
            return db.adminCommand({configureFailPoint: failpointName, mode: "off"});
        }
    };
    replSet.startSet();
    replSet.initiate();
    var db = replSet.getPrimary();
    setFailpointBool("hangAfterStartingIndexBuildUnlocked", true);

    // Blocks because of failpoint
    var join = startParallelShell("db.coll.createIndex({a: 1, b: 1}, {background: true})",
                                  replSet.ports[0]);

    // Let the createIndex start to run.
    IndexBuildTest.waitForIndexBuildToStart(db.getDB('test'), 'coll', 'a_1_b_1', {
        'locks.Global': {$exists: false},
        progress: {$exists: true},
    });

    // Repeated calls should continue to fail without crashing.
    assert.commandFailed(db.adminCommand({restartCatalog: 1}));
    assert.commandFailed(db.adminCommand({restartCatalog: 1}));
    assert.commandFailed(db.adminCommand({restartCatalog: 1}));

    // Unset failpoint so we can join the parallel shell.
    setFailpointBool("hangAfterStartingIndexBuildUnlocked", false);
    join();
    replSet.stopSet();
})();
