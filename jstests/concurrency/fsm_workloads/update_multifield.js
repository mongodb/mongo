'use strict';

/**
 * update_multifield.js
 *
 * Does updates that affect multiple fields on a single document.
 * The collection has an index for each field, and a compound index for all fields.
 */

// For isMongod and supportsDocumentLevelConcurrency.
load('jstests/concurrency/fsm_workload_helpers/server_types.js');

var $config = (function() {

    function makeQuery(options) {
        var query = {};
        if (!options.multi) {
            query._id = Random.randInt(options.numDocs);
        }

        if (options.isolated) {
            query.$isolated = 1;
        }

        return query;
    }

    // returns an update doc
    function makeRandomUpdateDoc() {
        var x = Random.randInt(5);
        var y = Random.randInt(5);
        // ensure z is never 0, so the $inc is never 0, so we can assert nModified === nMatched
        var z = Random.randInt(5) + 1;
        var set = Random.rand() > 0.5;
        var push = Random.rand() > 0.2;

        var updateDoc = {};
        updateDoc[set ? '$set' : '$unset'] = {x: x};
        updateDoc[push ? '$push' : '$pull'] = {y: y};
        updateDoc.$inc = {z: z};

        return updateDoc;
    }

    var states = {
        update: function update(db, collName) {
            // choose an update to apply
            var updateDoc = makeRandomUpdateDoc();

            // apply this update
            var query =
                makeQuery({multi: this.multi, isolated: this.isolated, numDocs: this.numDocs});
            var res = db[collName].update(query, updateDoc, {multi: this.multi});
            this.assertResult(res, db, collName, query);
        }
    };

    var transitions = {update: {update: 1}};

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({x: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({y: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({z: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({x: 1, y: 1, z: 1}));

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs = Math.floor(this.threadCount / 3);
        assertAlways.gt(this.numDocs, 0, 'numDocs should be a positive number');

        for (var i = 0; i < this.numDocs; ++i) {
            var res = db[collName].insert({_id: i});
            assertWhenOwnColl.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 10,
        iterations: 10,
        startState: 'update',
        states: states,
        transitions: transitions,
        data: {
            assertResult: function(res, db, collName, query) {
                assertAlways.eq(0, res.nUpserted, tojson(res));

                if (isMongod(db) && supportsDocumentLevelConcurrency(db)) {
                    // Storage engines which support document-level concurrency will automatically
                    // retry any operations when there are conflicts, so we should always see a
                    // matching document.
                    assertWhenOwnColl.eq(res.nMatched, 1, tojson(res));
                    if (db.getMongo().writeMode() === 'commands') {
                        assertWhenOwnColl.eq(res.nModified, 1, tojson(res));
                    }
                } else {
                    // On storage engines that do not support document-level concurrency, it is
                    // possible that the query will not find the document. This can happen if
                    // another thread updated the target document during a yield, triggering an
                    // invalidation.
                    assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
                    if (db.getMongo().writeMode() === 'commands') {
                        assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                        assertAlways.eq(res.nModified, res.nMatched, tojson(res));
                    }
                }
            },
            multi: false,
            isolated: false,
        },
        setup: setup
    };

})();
