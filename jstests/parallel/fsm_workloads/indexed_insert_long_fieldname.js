/**
 * indexed_insert_long_fieldname.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * field name is a long string.
 */
load('jstests/parallel/fsm_libs/runner.js'); // for extendWorkload
load('jstests/parallel/fsm_workloads/indexed_insert_base.js'); // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.indexedField = 'Supercalifragilisticexpialidocious';

    return $config;
});
