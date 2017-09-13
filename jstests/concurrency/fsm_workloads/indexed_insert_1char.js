'use strict';

/**
 * indexed_insert_1char.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is a 1-character string based on the thread's id.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.indexedField = 'indexed_insert_1char';
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.indexedValue = String.fromCharCode(33 + this.tid);
    };

    return $config;
});
