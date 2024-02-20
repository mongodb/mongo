/**
 * updateOne_update_queue.js
 *
 * A large number of documents are inserted during the workload setup. Each thread repeatedly
 * updates a document from the collection using the updateOne command with a sort. At the end of the
 * workload, the contents of the database are checked for whether the correct number of documents
 * were updated.
 *
 * This test is modeled off of findAndModify_update_queue.js, but instead of storing the _id field
 * of the updated document in another database and ensuring that every thread updated a different
 * document from the other threads, we check that the correct number of documents were updated
 * because updateOne doesn't return the modified document (and its _id value) unless upsert is true.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_80,
 * ]
 */
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function() {
    var data = {
        // Use the workload name as the database name, since the workload name is assumed to be
        // unique.
        uniqueDBName: jsTestName(),

        newDocForInsert: function newDocForInsert(i) {
            return {_id: i, a: 1, b: 1};
        },

        getIndexSpecs: function getIndexSpecs() {
            return [{a: 1}, {b: 1}];
        },

        opName: 'updated'
    };

    var states = (function() {
        function updateOne(db, collName) {
            const updateCmd = {
                update: collName,
                updates: [{q: {a: 1, b: 1}, u: {$inc: {a: 1}}, multi: false, sort: {a: -1}}]
            };

            // Update field 'a' to avoid matching the same document again.
            var res = db.runCommand(updateCmd);

            if (isMongod(db)) {
                // Storage engines should automatically retry the operation, and thus should never
                // return 0.
                // In the rare case that the retry limit is exceeded, a document may not be matched.
                assert.neq(
                    res.nModified,
                    0,
                    'updateOne should have found and updated a matching document, returned ' +
                        tojson(res));
            }
        }

        return {updateOne: updateOne};
    })();

    var transitions = {updateOne: {updateOne: 1}};

    function setup(db, collName) {
        // Each thread should update exactly one document per iteration.
        this.numDocs = this.iterations * this.threadCount;

        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            var doc = this.newDocForInsert(i);
            // Require that documents inserted by this workload use _id values that can be compared
            // using the default JS comparator.
            assert.neq(typeof doc._id,
                       'object',
                       'default comparator of' +
                           ' Array.prototype.sort() is not well-ordered for JS objects');
            bulk.insert(doc);
        }
        var res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);

        this.getIndexSpecs().forEach(function createIndex(indexSpec) {
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });
    }

    function teardown(db, collName) {
        var numOldDocs = db[collName].countDocuments({a: 1, b: 1});
        assert.eq(numOldDocs, 0);
        var numNewDocs = db[collName].countDocuments({a: 2, b: 1});
        assert.eq(numNewDocs, this.numDocs);
    }

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        startState: 'updateOne',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
