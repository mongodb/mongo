'use strict';

/**
 * update_multifield.js
 *
 * Does updates that affect multiple fields on a single document.
 * The collection has an index for each field, and a compound index for all fields.
 */

load('jstests/concurrency/fsm_workload_helpers/server_types.js'); // for isMongod and isMMAPv1

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
        updateDoc[set ? '$set' : '$unset'] = { x: x };
        updateDoc[push ? '$push' : '$pull'] = { y: y };
        updateDoc.$inc = { z: z };

        return updateDoc;
    }

    var states = {
        update: function update(db, collName) {
            // choose an update to apply
            var updateDoc = makeRandomUpdateDoc();

            // apply this update
            var query = makeQuery({
                multi: this.multi,
                isolated: this.isolated,
                numDocs: this.numDocs
            });
            var res = db[collName].update(query, updateDoc, { multi: this.multi });
            this.assertResult(res, db, collName, query);
        }
    };

    var transitions = {
        update: { update: 1 }
    };

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({ x: 1 }));
        assertAlways.commandWorked(db[collName].ensureIndex({ y: 1 }));
        assertAlways.commandWorked(db[collName].ensureIndex({ z: 1 }));
        assertAlways.commandWorked(db[collName].ensureIndex({ x: 1, y: 1, z: 1 }));

        for (var i = 0; i < this.numDocs; ++i) {
            var res = db[collName].insert({ _id: i });
            assertWhenOwnColl.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted);
        }
    }

    var threadCount = 10;
    return {
        threadCount: threadCount,
        iterations: 10,
        startState: 'update',
        states: states,
        transitions: transitions,
        data: {
            assertResult: function(res, db, collName, query) {
                assertAlways.eq(0, res.nUpserted, tojson(res));

                if (isMongod(db) && !isMMAPv1(db)) {
                    // For non-mmap storage engines we can have a strong assertion that exactly one
                    // doc will be modified.
                    assertWhenOwnColl.eq(res.nMatched, 1, tojson(res));
                    if (db.getMongo().writeMode() === 'commands') {
                        assertWhenOwnColl.eq(res.nModified, 1, tojson(res));
                    }
                }
                else {
                    // Zero matches are possible for MMAP v1 because the update will skip a document
                    // that was invalidated during a yield.
                    assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
                    if (db.getMongo().writeMode() === 'commands') {
                        assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                        assertAlways.eq(res.nModified, res.nMatched, tojson(res));
                    }
                }
            },
            multi: false,
            isolated: false,
            // numDocs should be much less than threadCount, to make more threads use the same docs
            numDocs: Math.floor(threadCount / 3)
        },
        setup: setup
    };

})();
