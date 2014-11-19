/**
 * indexed_insert_1char.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is a 1-character string based on the thread's id.
 */
load('jstests/parallel/fsm_libs/runner.js'); // for parseConfig
load('jstests/parallel/fsm_workloads/indexed_insert_base.js'); // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.states.init = function(db, collName) {
        $super.states.init.apply(this, arguments);

        this.indexedValue = String.fromCharCode(33 + this.tid);
    };

    return $config;
});
