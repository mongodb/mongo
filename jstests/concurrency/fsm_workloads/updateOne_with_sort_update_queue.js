/**
 * updateOne_update_queue.js
 *
 * A large number of documents are inserted during the workload setup. Each thread repeatedly
 * updates a document from the collection using the updateOne command with a sort. At the end of the
 * workload, the contents of the database are checked for whether the correct number of documents
 * were updated. They are also checked to ensure that the modified documents were those that came
 * first in the sort order (for a descending sort, the documents that had the highest sortField
 * values). While sortField is modified by the update, the document's _id value is the same as its
 * old sortField value, so we check the value at _id instead.
 *
 * This test is modeled off of findAndModify_update_queue.js, but instead of storing the _id field
 * of the updated document in another database and ensuring that every thread updated a different
 * document from the other threads, we check that the correct number of documents were updated
 * because updateOne doesn't return the modified document (and its _id value) unless upsert is true.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_81,
 * ]
 */
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function() {
    var data = {
        // Use the workload name as the database name, since the workload name is assumed to be
        // unique.
        uniqueDBName: jsTestName(),

        newDocForInsert: function newDocForInsert(i) {
            return {_id: i, sortField: i, queryField: 1};
        },

        getIndexSpecs: function getIndexSpecs() {
            return [{sortField: -1}, {queryField: 1}];
        },

        opName: 'updated'
    };

    var states = (function() {
        function updateOne(db, collName) {
            const updateCmd = {
                update: collName,
                updates: [{
                    q: {queryField: 1},
                    u: {$set: {sortField: -1, queryField: -1}},
                    multi: false,
                    sort: {sortField: -1}
                }]
            };

            // Update field 'sortField' to avoid matching the same document again.
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
        this.numDocsToMatch = this.iterations * this.threadCount;
        this.numDocsTotal = 2 * this.numDocsToMatch;

        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 1; i <= this.numDocsTotal; ++i) {
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
        assert.eq(this.numDocsTotal, res.nInserted);

        this.getIndexSpecs().forEach(function createIndex(indexSpec) {
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });
    }

    function teardown(db, collName) {
        var numNewDocs = db[collName].countDocuments({sortField: -1, queryField: -1});
        assert.eq(numNewDocs, this.numDocsToMatch);

        // Ensure that the modified documents were those that came first in the sort order (for a
        // descending sort, the documents that had the highest sortField values). While sortField is
        // modified by the update, the document's _id value is the same as its old sortField value,
        // so we check the value at _id instead.
        var docs = db[collName].find({sortField: -1, queryField: -1}).sort({_id: -1}).toArray();
        for (var i = 0; i < numNewDocs; ++i) {
            assert.eq(docs[i]._id, this.numDocsTotal - i);
        }
    }

    return {
        threadCount: 10,
        iterations: 50,
        data: data,
        startState: 'updateOne',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
