'use strict';

/**
 * update_rename.js
 *
 * Each thread does a $rename to cause documents to jump between indexes.
 */
var $config = (function() {

    var fieldNames = ['update_rename_x', 'update_rename_y', 'update_rename_z'];

    function choose(array) {
        assertAlways.gt(array.length, 0, "can't choose an element of an empty array");
        return array[Random.randInt(array.length)];
    }

    var states = {
        update: function update(db, collName) {
            var from = choose(fieldNames);
            var to = choose(fieldNames.filter(function(n) {
                return n !== from;
            }));
            var updater = {$rename: {}};
            updater.$rename[from] = to;

            var query = {};
            query[from] = {$exists: 1};

            var res = db[collName].update(query, updater);

            assertAlways.eq(0, res.nUpserted, tojson(res));
            assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
            if (db.getMongo().writeMode() === 'commands') {
                assertWhenOwnColl.eq(res.nMatched, res.nModified, tojson(res));
            }
        }
    };

    var transitions = {update: {update: 1}};

    function setup(db, collName, cluster) {
        // Create an index on all but one fieldName key to make it possible to test renames
        // between indexed fields and non-indexed fields
        fieldNames.slice(1).forEach(function(fieldName) {
            var indexSpec = {};
            indexSpec[fieldName] = 1;
            assertAlways.commandWorked(db[collName].ensureIndex(indexSpec));
        });

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs = Math.floor(this.threadCount / 5);
        assertAlways.gt(this.numDocs, 0, 'numDocs should be a positive number');

        for (var i = 0; i < this.numDocs; ++i) {
            var fieldName = fieldNames[i % fieldNames.length];
            var doc = {};
            doc[fieldName] = i;
            var res = db[collName].insert(doc);
            assertAlways.writeOK(res);
            assertAlways.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 20,
        iterations: 20,
        startState: 'update',
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
