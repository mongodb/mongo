'use strict';

/**
 * indexed_insert_unordered_bulk.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan.
 *
 * Uses an unordered, bulk operation to perform the inserts.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.indexedField = 'indexed_insert_unordered_bulk';
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.insert = function insert(db, collName) {
        var doc = {};
        doc[this.indexedField] = this.indexedValue;

        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.docsPerInsert; ++i) {
            bulk.insert(doc);
        }
        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.docsPerInsert, res.nInserted, tojson(res));

        this.nInserted += this.docsPerInsert;
    };

    $config.data.docsPerInsert = 15;

    return $config;
});
