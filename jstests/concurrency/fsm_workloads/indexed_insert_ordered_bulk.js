/**
 * indexed_insert_ordered_bulk.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan.
 *
 * Uses an ordered, bulk operation to perform the inserts.
 */
load('jstests/concurrency/fsm_libs/runner.js'); // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js'); // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.states.insert = function(db, collName) {
        var doc = {};
        doc[this.indexedField] = this.indexedValue;

        var bulk = db[collName].initializeOrderedBulkOp();
        for (var i = 0; i < this.docsPerInsert; ++i) {
            bulk.insert(doc);
        }
        assertWhenOwnColl.writeOK(bulk.execute());

        this.nInserted += this.docsPerInsert;
    };

    $config.data.docsPerInsert = 15;

    return $config;
});
