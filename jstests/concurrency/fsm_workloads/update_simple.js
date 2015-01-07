'use strict';

/**
 * update_simple.js
 *
 * Creates several docs. On each iteration, each thread chooses:
 *  - a random doc
 *  - whether to $set or $unset its field
 *  - what value to $set the field to
 */
var $config = (function() {

    var states = {
        set: function set(db, collName) {
            this.setOrUnset(db, collName, true, this.numDocs);
        },

        unset: function unset(db, collName) {
            this.setOrUnset(db, collName, false, this.numDocs);
        }
    };

    var transitions = {
        set: {
            set: 0.5,
            unset: 0.5
        },
        unset: {
            set: 0.5,
            unset: 0.5
        }
    };

    function setup(db, collName) {
        // index on 'value', the field being updated
        assertAlways.commandWorked(db[collName].ensureIndex({ value: 1 }));
        for (var i = 0; i < this.numDocs; ++i) {
            // make sure the inserted docs have a 'value' field, so they won't need
            // to grow when this workload runs against a capped collection
            var res = db[collName].insert({ _id: i, value: 0 });
            assertWhenOwnColl.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted);
        }
    }

    var threadCount = 20;
    return {
        threadCount: threadCount,
        iterations: 20,
        startState: 'set',
        states: states,
        transitions: transitions,
        data: {
            // explicitly pass db to avoid accidentally using the global `db`
            assertResult: function assertResult(db, res) {
                assertAlways.eq(0, res.nUpserted, tojson(res));
                assertWhenOwnColl.eq(1, res.nMatched, tojson(res));
                if (db.getMongo().writeMode() === 'commands') {
                    assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                }
            },

            setOrUnset: function setOrUnset(db, collName, set, numDocs) {
                // choose a doc and value to use in the update
                var docIndex = Random.randInt(numDocs);
                var value = Random.randInt(5);

                var updater = {};
                updater[set ? '$set' : '$unset'] = { value: value };

                var query = { _id: docIndex };
                var res = this.doUpdate(db, collName, query, updater);
                this.assertResult(db, res);
            },

            doUpdate: function doUpdate(db, collName, query, updater) {
                return db[collName].update(query, updater);
            },

            // numDocs should be much less than threadCount, to make more threads use the same docs
            numDocs: Math.floor(threadCount / 5)
        },
        setup: setup
    };

})();
