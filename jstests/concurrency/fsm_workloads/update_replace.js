'use strict';

/**
 * update_replace.js
 *
 * Does updates that replace an entire document.
 * The collection has indexes on some but not all fields.
 */
var $config = (function() {

    // explicitly pass db to avoid accidentally using the global `db`
    function assertResult(db, res) {
        assertAlways.eq(0, res.nUpserted, tojson(res));
        assertWhenOwnColl.eq(1, res.nMatched,  tojson(res));
        if (db.getMongo().writeMode() === 'commands') {
            assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
        }
    }

    // returns an update doc
    function getRandomUpdateDoc() {
        var choices = [
            {},
            { x: 1, y: 1, z: 1 },
            { a: 1, b: 1, c: 1 }
        ];
        return choices[Random.randInt(choices.length)];
    }

    var states = {
        update: function update(db, collName) {
            // choose a doc to update
            var docIndex = Random.randInt(this.numDocs);

            // choose an update to apply
            var updateDoc = getRandomUpdateDoc();

            // apply the update
            var res = db[collName].update({ _id: docIndex }, updateDoc);
            assertResult(db, res);
        }
    };

    var transitions = {
        update: { update: 1 }
    };

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({ a: 1 }));
        assertAlways.commandWorked(db[collName].ensureIndex({ b: 1 }));
        // no index on c

        assertAlways.commandWorked(db[collName].ensureIndex({ x: 1 }));
        assertAlways.commandWorked(db[collName].ensureIndex({ y: 1 }));
        // no index on z

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs =  Math.floor(this.threadCount / 3);
        assertAlways.gt(this.numDocs, 0, 'numDocs should be a positive number');

        for (var i = 0; i < this.numDocs; ++i) {
            var res = db[collName].insert({ _id: i });
            assertWhenOwnColl.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted);
        }

        assertWhenOwnColl.eq(this.numDocs, db[collName].find().itcount());
    }

    return {
        threadCount: 10,
        iterations: 10,
        startState: 'update',
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
