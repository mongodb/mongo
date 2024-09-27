/**
 * indexed_insert_upsert.js
 *
 * Inserts documents into an indexed collection and asserts that the documents
 * appear in both a collection scan and an index scan. The indexed value is a
 * number, the thread id.
 *
 * Instead of inserting via coll.insert(), this workload inserts using an
 * upsert.
 * @tags: [
 *   requires_getmore
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.indexedField = 'indexed_insert_upsert';
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.counter = 0;
    };

    $config.states.insert = function insert(db, collName) {
        var doc = this.getDoc();
        doc.counter = this.counter++;  // ensure doc is unique to guarantee an upsert occurs
        doc._id = new ObjectId();      // _id is required for shard targeting

        var res = db[collName].update(doc, {$inc: {unused: 0}}, {upsert: true});
        assert.eq(0, res.nMatched, tojson(res));
        assert.eq(1, res.nUpserted, tojson(res));
        assert.eq(0, res.nModified, tojson(res));

        this.nInserted += this.docsPerInsert;
    };

    return $config;
});
