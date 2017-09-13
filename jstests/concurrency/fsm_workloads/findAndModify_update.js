'use strict';

/**
 * findAndModify_update.js
 *
 * Each thread inserts multiple documents into a collection, and then
 * repeatedly performs the findAndModify command. A single document is
 * selected based on 'query' and 'sort' specifications, and updated
 * using either the $min or $max operator.
 */
var $config = (function() {

    var data = {
        numDocsPerThread: 3,  // >1 for 'sort' to be meaningful
        shardKey: {tid: 1}
    };

    var states = (function() {

        function makeDoc(tid) {
            return {_id: new ObjectId(), tid: tid, value: 0};
        }

        function init(db, collName) {
            for (var i = 0; i < this.numDocsPerThread; ++i) {
                var res = db[collName].insert(makeDoc(this.tid));
                assertAlways.writeOK(res);
                assertAlways.eq(1, res.nInserted);
            }
        }

        function findAndModifyAscending(db, collName) {
            var updatedValue = this.tid;

            var res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {value: 1},
                update: {$max: {value: updatedValue}},
                new: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null, 'query spec should have matched a document');

            if (doc !== null) {
                assertAlways.eq(this.tid, doc.tid);
                assertWhenOwnColl.eq(updatedValue, doc.value);
            }
        }

        function findAndModifyDescending(db, collName) {
            var updatedValue = -this.tid;

            var res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {value: -1},
                update: {$min: {value: updatedValue}},
                new: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null, 'query spec should have matched a document');

            if (doc !== null) {
                assertAlways.eq(this.tid, doc.tid);
                assertWhenOwnColl.eq(updatedValue, doc.value);
            }
        }

        return {
            init: init,
            findAndModifyAscending: findAndModifyAscending,
            findAndModifyDescending: findAndModifyDescending
        };

    })();

    var transitions = {
        init: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5},
        findAndModifyAscending: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5},
        findAndModifyDescending: {findAndModifyAscending: 0.5, findAndModifyDescending: 0.5}
    };

    function setup(db, collName, cluster) {
        var res = db[collName].ensureIndex({tid: 1, value: 1});
        assertAlways.commandWorked(res);
    }

    return {
        threadCount: 20,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
