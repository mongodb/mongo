'use strict';

/**
 * indexed_insert_2dsphere.js
 *
 * Inserts documents into an indexed collection and asserts that the documents
 * appear in both a collection scan and an index scan. The indexed value is a
 * legacy coordinate pair, indexed with a 2dsphere index.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_2d.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.indexedField = 'indexed_insert_2dsphere';

    $config.data.getIndexSpec = function getIndexSpec() {
        var ixSpec = {};
        ixSpec[this.indexedField] = '2dsphere';
        return ixSpec;
    };

    return $config;
});
