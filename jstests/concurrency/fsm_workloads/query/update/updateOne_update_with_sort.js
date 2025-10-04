/**
 * updateOne_update_with_sort.js
 *
 * Ensures that updateOne will not apply an update to a document which, due to a concurrent
 * modification, no longer matches the query predicate. At the end of the workload, the contents of
 * the database are checked for whether the correct number of documents were updated.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_81,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/update/updateOne_with_sort_update_queue.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use the same workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = jsTestName();

    $config.iterations = 1;

    $config.states.updateOne = function (db, collName) {
        const updateCmd = {
            update: collName,
            updates: [
                {
                    q: {sortField: 1, queryField: 1},
                    u: {$inc: {sortField: 1}},
                    multi: false,
                    sort: {sortField: -1},
                },
            ],
        };

        // Update field 'a' to avoid matching the same document again.
        let res = db.runCommand(updateCmd);
        if (isMongod(db)) {
            assert.contains(res.nModified, [0, 1], tojson(res));
        }
    };

    $config.setup = function (db, collName) {
        let bulk = db[collName].initializeUnorderedBulkOp();
        let doc = this.newDocForInsert(1);
        // Require that documents inserted by this workload use _id values that can be compared
        // using the default JS comparator.
        assert.neq(
            typeof doc._id,
            "object",
            "default comparator of" + " Array.prototype.sort() is not well-ordered for JS objects",
        );
        bulk.insert(doc);
        let res = bulk.execute();
        assert.commandWorked(res);
        // Insert a single document into the collection.
        assert.eq(1, res.nInserted);

        this.getIndexSpecs().forEach(function createIndex(indexSpec) {
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });
    };

    $config.teardown = function (db, collName) {
        let docs = db[collName].find().toArray();
        // Assert that while 10 threads attempted an updateOne on a single matching document, it was
        // only updated once with the correct update. All updateOne operations look for a document
        // with sortField==1, and then increment 'sortField' by 1. One should win the race and set
        // sortField=2. The others should fail to find a match. The assertion is that 'sortField'
        // got incremented once (not zero times and not twice).
        assert.eq(docs.length, 1);
        assert.eq(docs[0].sortField, 2);
    };

    return $config;
});
