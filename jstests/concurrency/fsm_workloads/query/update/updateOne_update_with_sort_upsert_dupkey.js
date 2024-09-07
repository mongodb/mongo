/**
 * updateOne_update_with_sort_upsert_dupkey.js
 *
 * Ensures that updateOne with a sort, {upsert: true}, and a predicate on the same _id value will
 * perform an upsert when no documents match the query predicate. Verify that when there is a
 * WriteConflict, the update command is retried, and the document with the matching _id value is
 * updated. At the end of the workload, the contents of the database are checked for whether the
 * documents were updated correctly.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_81,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/update/updateOne_update_with_sort_and_upsert.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Use the same workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = jsTestName();

    $config.data.getIndexSpecs = function getIndexSpecs() {
        return [];
    };

    $config.threadCount = 2;

    // The query predicate is on the _id unique index so that we
    $config.states.updateOne = function(db, collName) {
        const updateCmd = {
            update: collName,
            updates: [{
                q: {_id: 2},
                u: {$inc: {sortField: 1}},
                multi: false,
                sort: {sortField: -1},
                upsert: true
            }]
        };

        var res = db.runCommand(updateCmd);
        if (isMongod(db)) {
            if (res.hasOwnProperty('upserted') && res.upserted.length != 0) {
                // Case 1: The _id value is not yet present, so a new document is added to the
                // collection.
                assert.eq(res.nModified, 0, tojson(res));
            } else {
                // Case 2: The _id value matches an existing document in the collection, so it
                // modifies that document.
                assert.eq(res.nModified, 1, tojson(res));
            }
        }
    };

    $config.teardown = function(db, collName) {
        var docs = db[collName].find().toArray();

        // Assert that when 2 threads attempt an updateOne with an upsert on a query with the same
        // _id value (unique key), the earlier thread successfully upserts the document when it is
        // not found. Since the query predicate contains no initial value for 'sortField', it adds
        // the document {_id: 2, sortField: 1}. The second thread then finds the newly upserted
        // document and updates the field 'sortField', updating the document to {_id: 2, sortField:
        // 2}.
        assert.eq(docs.length, 2);
        var modifiedDoc = db[collName].find({_id: 2}).toArray();
        assert.eq(modifiedDoc[0].sortField, this.numDocs);
    };

    return $config;
});
