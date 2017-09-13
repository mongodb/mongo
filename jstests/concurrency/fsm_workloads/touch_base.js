'use strict';

/**
 * touch_base.js
 *
 * Bulk inserts documents in batches of 100, uses the touch command on "data" and "index",
 * and queries to verify the number of documents inserted by the thread.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_where.js');  // for $config
// For isMongod, isMMAPv1, and isEphemeral.
load('jstests/concurrency/fsm_workload_helpers/server_types.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.generateDocumentToInsert = function generateDocumentToInsert() {
        return {tid: this.tid, x: Random.randInt(10)};
    };

    $config.data.generateTouchCmdObj = function generateTouchCmdObj(collName) {
        return {touch: collName, data: true, index: true};
    };

    $config.states.touch = function touch(db, collName) {
        var res = db.runCommand(this.generateTouchCmdObj(collName));
        if (isMongod(db) && (isMMAPv1(db) || isEphemeral(db))) {
            assertAlways.commandWorked(res);
        } else {
            // SERVER-16850 and SERVER-16797
            assertAlways.commandFailed(res);
        }
    };

    $config.states.query = function query(db, collName) {
        var count = db[collName].find({tid: this.tid}).itcount();
        assertWhenOwnColl.eq(count,
                             this.insertedDocuments,
                             'collection scan should return the number of documents this thread' +
                                 ' inserted');
    };

    $config.transitions = {
        insert: {insert: 0.2, touch: 0.4, query: 0.4},
        touch: {insert: 0.4, touch: 0.2, query: 0.4},
        query: {insert: 0.4, touch: 0.4, query: 0.2}
    };

    $config.setup = function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({x: 1}));
    };

    return $config;
});
