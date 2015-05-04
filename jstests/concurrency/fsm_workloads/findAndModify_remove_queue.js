'use strict';

/**
 * findAndModify_remove_queue.js
 *
 * A large number of documents are inserted during the workload setup. Each thread repeated removes
 * a document from the collection using the findAndModify command, and stores the _id field of that
 * document in another database. At the end of the workload, the contents of the other database are
 * checked for whether one thread removed the same document as another thread.
 *
 * This workload was designed to reproduce SERVER-18304.
 */
var $config = (function() {

    var data =  {
        numDocs: 1000,

        // Use the workload name as the database name, since the workload name is assumed to be
        // unique.
        uniqueDBName: 'findAndModify_remove_queue',

        newDocForInsert: function newDocForInsert(i) {
            return { _id: i, rand: Random.rand() };
        },

        getIndexSpec: function getIndexSpec() {
            return { rand: 1 };
        },

        opName: 'removed',

        saveDocId: function saveDocId(db, collName, id) {
            // Use a separate database to avoid conflicts with other FSM workloads.
            var ownedDB = db.getSiblingDB(this.uniqueDBName);

            var updateDoc = { $push: {} };
            updateDoc.$push[this.opName] = id;

            var res = ownedDB[collName].update({ _id: this.tid }, updateDoc, { upsert: true });
            assertAlways.writeOK(res);

            assertAlways.contains(res.nMatched, [0, 1], tojson(res));
            if (res.nMatched === 0) {
                if (ownedDB.getMongo().writeMode() === 'commands') {
                    assertAlways.eq(0, res.nModified, tojson(res));
                }
                assertAlways.eq(1, res.nUpserted, tojson(res));
            }
            else {
                if (ownedDB.getMongo().writeMode() === 'commands') {
                    assertAlways.eq(1, res.nModified, tojson(res));
                }
                assertAlways.eq(0, res.nUpserted, tojson(res));
            }
        }
    };

    var states = (function() {

        function remove(db, collName) {
            var res = db.runCommand({
                findAndModify: db[collName].getName(),
                query: {},
                sort: { rand: -1 },
                remove: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            if (doc !== null) {
                this.saveDocId.call(this, db, collName, doc._id);
            }
        }

        return {
            remove: remove
        };

    })();

    var transitions = {
        remove: { remove: 1 }
    };

    function setup(db, collName, cluster) {
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            var doc = this.newDocForInsert(i);
            // Require that documents inserted by this workload use _id values that can be compared
            // using the default JS comparator.
            assertAlways.neq(typeof doc._id, 'object', 'default comparator of' +
                             ' Array.prototype.sort() is not well-ordered for JS objects');
            bulk.insert(doc);
        }
        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.numDocs, res.nInserted);

        assertAlways.commandWorked(db[collName].ensureIndex(this.getIndexSpec()));
    }

    function teardown(db, collName, cluster) {
        var ownedDB = db.getSiblingDB(this.uniqueDBName);

        assertWhenOwnColl(function() {
            var docs = ownedDB[collName].find().toArray();
            var ids = [];

            for (var i = 0; i < docs.length; ++i) {
                ids.push(docs[i][this.opName].sort());
            }

            checkForDuplicateIds(ids, this.opName);
        }.bind(this));

        var res = ownedDB.dropDatabase();
        assertAlways.commandWorked(res);
        assertAlways.eq(this.uniqueDBName, res.dropped);

        function checkForDuplicateIds(ids, opName) {
            var indices = new Array(ids.length);
            for (var i = 0; i < indices.length; ++i) {
                indices[i] = 0;
            }

            while (true) {
                var smallest = findSmallest(ids, indices);
                if (smallest === null) {
                    break;
                }

                var msg = 'threads ' + tojson(smallest.indices) +
                          ' claim to have ' + opName +
                          ' a document with _id = ' + tojson(smallest.value);
                assertWhenOwnColl.eq(1, smallest.indices.length, msg);

                indices[smallest.indices[0]]++;
            }
        }

        function findSmallest(arrays, indices) {
            var smallestValueIsSet = false;
            var smallestValue;
            var smallestIndices;

            for (var i = 0; i < indices.length; ++i) {
                if (indices[i] >= arrays[i].length) {
                    continue;
                }

                var value = arrays[i][indices[i]];
                if (!smallestValueIsSet || value < smallestValue) {
                    smallestValueIsSet = true;
                    smallestValue = value;
                    smallestIndices = [i];
                }
                else if (value === smallestValue) {
                    smallestIndices.push(i);
                }
            }

            if (!smallestValueIsSet) {
                return null;
            }
            return { value: smallestValue, indices: smallestIndices };
        }
    }

    var threadCount = 10;
    return {
        threadCount: threadCount,
        iterations: Math.floor(data.numDocs / threadCount),
        data: data,
        startState: 'remove',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };

})();
