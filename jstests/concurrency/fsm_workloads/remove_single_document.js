'use strict';

/**
 * remove_single_document.js
 *
 * Repeatedly remove a document from the collection.
 */
var $config = (function() {

    var states = {
        remove: function remove(db, collName) {
            // try removing a random document
            var res = this.doRemove(db, collName, {rand: {$gte: Random.rand()}}, {justOne: true});
            assertAlways.lte(res.nRemoved, 1);
            if (res.nRemoved === 0) {
                // The above remove() can fail to remove a document when the random value
                // in the query is greater than any of the random values in the collection.
                // When that situation occurs, just remove an arbitrary document instead.
                res = this.doRemove(db, collName, {}, {justOne: true});
                assertAlways.lte(res.nRemoved, 1);
            }
            this.assertResult(res);
        }
    };

    var transitions = {remove: {remove: 1}};

    function setup(db, collName, cluster) {
        // insert enough documents so that each thread can remove exactly one per iteration
        var num = this.threadCount * this.iterations;
        for (var i = 0; i < num; ++i) {
            db[collName].insert({i: i, rand: Random.rand()});
        }
        assertWhenOwnColl.eq(db[collName].find().itcount(), num);
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        transitions: transitions,
        setup: setup,
        data: {
            doRemove: function doRemove(db, collName, query, options) {
                return db[collName].remove(query, options);
            },
            assertResult: function assertResult(res) {
                assertAlways.writeOK(res);
                // when running on its own collection,
                // this iteration should remove exactly one document
                assertWhenOwnColl.eq(1, res.nRemoved, tojson(res));
            }
        },
        startState: 'remove'
    };

})();
