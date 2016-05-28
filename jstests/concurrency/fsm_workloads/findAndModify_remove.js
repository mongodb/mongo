'use strict';

/**
 * findAndModify_remove.js
 *
 * Each thread repeatedly inserts a document, and subsequently performs
 * the findAndModify command to remove it.
 */
var $config = (function() {

    var data = {shardKey: {tid: 1}};

    var states = (function() {

        function init(db, collName) {
            this.iter = 0;
        }

        function insertAndRemove(db, collName) {
            var res = db[collName].insert({tid: this.tid, value: this.iter});
            assertAlways.writeOK(res);
            assertAlways.eq(1, res.nInserted);

            res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {iter: -1},
                remove: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null, 'query spec should have matched a document');

            if (doc !== null) {
                assertAlways.eq(this.tid, doc.tid);
                assertWhenOwnColl.eq(this.iter, doc.value);
            }

            this.iter++;
        }

        return {init: init, insertAndRemove: insertAndRemove};

    })();

    var transitions = {init: {insertAndRemove: 1}, insertAndRemove: {insertAndRemove: 1}};

    return {threadCount: 20, iterations: 20, data: data, states: states, transitions: transitions};

})();
