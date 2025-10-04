/**
 * findAndModify_remove_queue.js
 *
 * A large number of documents are inserted during the workload setup. Each thread repeatedly
 * removes a document from the collection using the findAndModify command, and stores the _id field
 * of that document in another database. At the end of the workload, the contents of the other
 * database are checked for whether one thread removed the same document as another thread.
 *
 * This workload was designed to reproduce SERVER-18304.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1.
 *   requires_fcv_71,
 * ]
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        // Use the workload name as the database name, since the workload name is assumed to be
        // unique.
        uniqueDBName: jsTestName(),

        newDocForInsert: function newDocForInsert(i) {
            return {_id: i, rand: Random.rand()};
        },

        getIndexSpecs: function getIndexSpecs() {
            return [{rand: 1}];
        },

        opName: "removed",

        saveDocId: function saveDocId(db, collName, id) {
            // Use a separate database to avoid conflicts with other FSM workloads.
            let ownedDB = db.getSiblingDB(db.getName() + this.uniqueDBName);

            let updateDoc = {$push: {}};
            updateDoc.$push[this.opName] = id;

            let res = ownedDB[collName].update({_id: this.tid}, updateDoc, {upsert: true});
            assert.commandWorked(res);

            assert.contains(res.nMatched, [0, 1], tojson(res));
            if (res.nMatched === 0) {
                assert.eq(0, res.nModified, tojson(res));
                assert.eq(1, res.nUpserted, tojson(res));
            } else {
                assert.eq(1, res.nModified, tojson(res));
                assert.eq(0, res.nUpserted, tojson(res));
            }
        },
    };

    let states = (function () {
        function remove(db, collName) {
            let res = db.runCommand({findAndModify: db[collName].getName(), query: {}, sort: {rand: -1}, remove: true});
            assert.commandWorked(res);

            let doc = res.value;
            if (isMongod(db)) {
                // Storage engines should automatically retry the operation, and thus should never
                // return null.
                assert.neq(doc, null, "findAndModify should have found and removed a matching document");
            }
            if (doc !== null) {
                this.saveDocId(db, collName, doc._id);
            }
        }

        return {remove: remove};
    })();

    let transitions = {remove: {remove: 1}};

    function setup(db, collName, cluster) {
        // Each thread should remove exactly one document per iteration.
        this.numDocs = this.iterations * this.threadCount;

        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            let doc = this.newDocForInsert(i);
            // Require that documents inserted by this workload use _id values that can be compared
            // using the default JS comparator.
            assert.neq(
                typeof doc._id,
                "object",
                "default comparator of" + " Array.prototype.sort() is not well-ordered for JS objects",
            );
            bulk.insert(doc);
        }
        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);

        this.getIndexSpecs().forEach(function createIndex(indexSpec) {
            assert.commandWorked(db[collName].createIndex(indexSpec));
        });
    }

    function teardown(db, collName, cluster) {
        let ownedDB = db.getSiblingDB(db.getName() + this.uniqueDBName);

        if (this.opName === "removed") {
            // Each findAndModify should be internally retried until it removes exactly one
            // document. Since this.numDocs == this.iterations * this.threadCount, there should not
            // be any documents remaining.
            assert.eq(db[collName].find().itcount(), 0, "Expected all documents to have been removed");
        } else if (this.opName === "updated") {
            // Each findAndModify should be internally retried until it updates exactly one
            // document. Since this.numDocs == this.iterations * this.threadCount, there should not
            // be any documents remaining with '{counter: 0}'.
            assert.eq(db[collName].find({counter: 0}).itcount(), 0, "Expected all documents to have been updated");
        }

        let docs = ownedDB[collName].find().toArray();
        let ids = [];

        for (let i = 0; i < docs.length; ++i) {
            ids.push(docs[i][this.opName].sort());
        }

        checkForDuplicateIds(ids, this.opName);

        assert.commandWorked(ownedDB.dropDatabase());

        function checkForDuplicateIds(ids, opName) {
            let indices = new Array(ids.length);
            for (let i = 0; i < indices.length; ++i) {
                indices[i] = 0;
            }

            while (true) {
                let smallest = findSmallest(ids, indices);
                if (smallest === null) {
                    break;
                }

                let msg =
                    "threads " +
                    tojson(smallest.indices) +
                    " claim to have " +
                    opName +
                    " a document with _id = " +
                    tojson(smallest.value);
                assert.eq(1, smallest.indices.length, msg);

                indices[smallest.indices[0]]++;
            }
        }

        function findSmallest(arrays, indices) {
            let smallestValueIsSet = false;
            let smallestValue;
            let smallestIndices;

            for (let i = 0; i < indices.length; ++i) {
                if (indices[i] >= arrays[i].length) {
                    continue;
                }

                let value = arrays[i][indices[i]];
                if (!smallestValueIsSet || value < smallestValue) {
                    smallestValueIsSet = true;
                    smallestValue = value;
                    smallestIndices = [i];
                } else if (value === smallestValue) {
                    smallestIndices.push(i);
                }
            }

            if (!smallestValueIsSet) {
                return null;
            }
            return {value: smallestValue, indices: smallestIndices};
        }
    }

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        startState: "remove",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
