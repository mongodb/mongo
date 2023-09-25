/**
 * findAndModify_remove.js
 *
 * Each thread repeatedly inserts a document, and subsequently performs
 * the findAndModify command to remove it.
 */
import {assertAlways, assertWhenOwnColl} from "jstests/concurrency/fsm_libs/assert.js";

export const $config = (function() {
    var data = {shardKey: {tid: 1}};

    var states = (function() {
        function init(db, collName) {
            this.iter = 0;
        }

        function insertAndRemove(db, collName) {
            var res = db[collName].insert({tid: this.tid, value: this.iter});
            assertAlways.commandWorked(res);
            assertAlways.eq(1, res.nInserted);

            res = db.runCommand({
                findandmodify: db[collName].getName(),
                query: {tid: this.tid},
                sort: {iter: -1},
                remove: true
            });
            assertAlways.commandWorked(res);

            var doc = res.value;
            assertWhenOwnColl(doc !== null,
                              'query spec should have matched a document, returned ' + tojson(res));

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
