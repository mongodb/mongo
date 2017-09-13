'use strict';

/**
 * indexed_insert_long_fieldname.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * field name is a long string.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // TODO: make this field name even longer?
    var length = 100;
    var prefix = 'indexed_insert_long_fieldname_';
    $config.data.indexedField = prefix + new Array(length - prefix.length + 1).join('x');
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    return $config;
});
