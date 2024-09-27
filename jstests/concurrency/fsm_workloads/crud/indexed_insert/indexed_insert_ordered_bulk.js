/**
 * indexed_insert_ordered_bulk.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan.
 *
 * Uses an ordered, bulk operation to perform the inserts.
 * @tags: [
 *   requires_getmore
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.indexedField = 'indexed_insert_ordered_bulk';
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.insert = function insert(db, collName) {
        var doc = {};
        doc[this.indexedField] = this.indexedValue;

        var bulk = db[collName].initializeOrderedBulkOp();
        for (var i = 0; i < this.docsPerInsert; ++i) {
            bulk.insert(doc);
        }
        var res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.docsPerInsert, res.nInserted, tojson(res));

        this.nInserted += this.docsPerInsert;
    };

    $config.data.docsPerInsert = 15;

    return $config;
});
