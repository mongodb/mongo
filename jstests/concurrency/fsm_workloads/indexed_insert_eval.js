'use strict';

/**
 * indexed_insert_eval.js
 *
 * Inserts multiple documents into an indexed collection using the eval command.
 * Asserts that all documents appear in both a collection scan and an index
 * scan. The indexed value is the thread id.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.nolock = false;

    $config.states.insert = function insert(db, collName) {
        var evalResult = db.runCommand({
            eval: function(collName, doc) {
                var insertResult = db[collName].insert(doc);
                return tojson(insertResult);
            },
            args: [collName, this.getDoc()],
            nolock: this.nolock
        });
        assertAlways.commandWorked(evalResult);
        var insertResult = JSON.parse(evalResult.retval);
        assertAlways.eq(1, insertResult.nInserted, tojson(insertResult));
        this.nInserted += this.docsPerInsert;
    };

    return $config;
});
