/**
 * updateOne_update_with_sort_and_upsert.js
 *
 * Ensures that updateOne with a sort and {upsert: true} will perform an upsert when, due to a
 * concurrent modification, no documents match the query predicate. At the end of the workload, the
 * contents of the database are checked for whether the correct number of documents were updated.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_80,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/updateOne_with_sort_update_queue.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Use the same workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = jsTestName();

    $config.iterations = 1;

    $config.states.updateOne = function(db, collName) {
        const updateCmd = {
            update: collName,
            updates:
                [{q: {a: 1, b: 1}, u: {$inc: {a: 1}}, multi: false, sort: {a: -1}, upsert: true}]
        };

        // Update field 'a' to avoid matching the same document again.
        var res = db.runCommand(updateCmd);
        if (isMongod(db)) {
            if (res.hasOwnProperty('upserted') && res.upserted.length != 0) {
                assert.eq(res.nModified, 0, tojson(res));
            } else {
                assert.eq(res.nModified, 1, tojson(res));
            }
        }
    };

    $config.setup = function(db, collName) {
        this.numDocs = this.iterations * this.threadCount;

        var bulk = db[collName].initializeUnorderedBulkOp();
        var doc = this.newDocForInsert(0);
        // Require that documents inserted by this workload use _id values that can be compared
        // using the default JS comparator.
        assert.neq(typeof doc._id,
                   'object',
                   'default comparator of' +
                       ' Array.prototype.sort() is not well-ordered for JS objects');
        bulk.insert(doc);
        var res = bulk.execute();
        assert.commandWorked(res);
        // Insert a single document into the collection.
        assert.eq(1, res.nInserted);

        this.getIndexSpecs().forEach(function createIndex(indexSpec) {
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });
    };

    $config.teardown = function(db, collName) {
        var docs = db[collName].find().toArray();
        // Assert that while 10 threads attempted an updateOne on a single matching document, it was
        // only updated once with the correct update. All updateOne operations look for a document
        // with a==1, and then increment 'a' by 1. One should win the race and set a=2. The others
        // should fail to find a match and will insert a new document instead. The assertion is that
        // 'a' got incremented once in the original document that was in the collection (not zero
        // times and not twice). The additional documents are the product of upserts and have unique
        // _id values.
        assert.eq(docs.length, this.numDocs);

        var seenIds = new Set();
        for (var i = 0; i < docs.length; i++) {
            assert.eq(docs[i].a, 2);
            assert.eq(seenIds.has(docs[i]._id), false);
            seenIds.add(docs[i]._id);
        }
        assert(seenIds.has(0));
    };

    return $config;
});
