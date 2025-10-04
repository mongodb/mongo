/**
 * findAndModify_mixed_queue_unindexed.js
 *
 * This workload is a combination of findAndModify_remove_queue_unindexed.js and
 * findAndModify_update_queue_unindexed.js.
 *
 * Each thread contends on the same document as one another by randomly performing either a
 * findAndModify update operation or a findAndModify remove operation. The lack of an index that
 * could satisfy the sort forces the findAndModify operations to scan all the matching documents in
 * order to find the relevant document. This increases the amount of work each findAndModify
 * operation has to do before getting to the matching document, and thus increases the chance of a
 * write conflict because each is trying to update or remove the same document.
 *
 * This workload was designed to reproduce SERVER-21434.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1.
 *   requires_fcv_71,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/findAndModify/findAndModify_remove_queue.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use the workload name as the database name, since the workload name is assumed to be
    // unique.
    $config.data.uniqueDBName = jsTestName();

    $config.data.newDocForInsert = function newDocForInsert(i) {
        return {_id: i, rand: Random.rand(), counter: 0};
    };

    $config.data.getIndexSpecs = function getIndexSpecs() {
        return [];
    };

    $config.data.opName = "modified";

    $config.data.validateResult = function validateResult(db, collName, res) {
        assert.commandWorked(res);

        let doc = res.value;
        if (isMongod(db)) {
            // Storage engines should automatically retry the operation, and thus should never
            // return null.
            assert.neq(doc, null, "findAndModify should have found a matching document");
        }
        if (doc !== null) {
            this.saveDocId(db, collName, doc._id);
        }
    };

    $config.states = (function () {
        // Avoid removing documents that were already updated.
        function remove(db, collName) {
            let res = db.runCommand({
                findAndModify: db[collName].getName(),
                query: {counter: 0},
                sort: {rand: -1},
                remove: true,
            });
            this.validateResult(db, collName, res);
        }

        function update(db, collName) {
            // Update the counter field to avoid matching the same document again.
            let res = db.runCommand({
                findAndModify: db[collName].getName(),
                query: {counter: 0},
                sort: {rand: -1},
                update: {$inc: {counter: 1}},
                new: false,
            });
            this.validateResult(db, collName, res);
        }

        return {
            remove: remove,
            update: update,
        };
    })();

    $config.transitions = {
        remove: {remove: 0.5, update: 0.5},
        update: {remove: 0.5, update: 0.5},
    };

    return $config;
});
