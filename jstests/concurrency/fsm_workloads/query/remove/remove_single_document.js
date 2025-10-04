/**
 * remove_single_document.js
 *
 * Repeatedly remove a document from the collection.
 *
 * @tags: [assumes_balancer_off]
 */
export const $config = (function () {
    let states = {
        remove: function remove(db, collName) {
            // try removing a random document
            let res = assert.commandWorked(this.doRemove(db, collName, {rand: {$gte: Random.rand()}}, {justOne: true}));

            assert.lte(res.nRemoved, 1, res);
            if (res.nRemoved === 0) {
                // The above remove() can fail to remove a document when the random value
                // in the query is greater than any of the random values in the collection.
                // When that situation occurs, just remove an arbitrary document instead.
                res = assert.commandWorked(this.doRemove(db, collName, {}, {justOne: true}));
                assert.lte(res.nRemoved, 1, res);
            }
            this.assertResult(res);
        },
    };

    let transitions = {remove: {remove: 1}};

    function setup(db, collName, cluster) {
        // insert enough documents so that each thread can remove exactly one per iteration
        let num = this.threadCount * this.iterations;
        for (let i = 0; i < num; ++i) {
            db[collName].insert({i: i, rand: Random.rand()});
        }
        assert.eq(db[collName].find().itcount(), num);
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
                assert.commandWorked(res);
                // when running on its own collection,
                // this iteration should remove exactly one document
                assert.eq(1, res.nRemoved, tojson(res));
            },
        },
        startState: "remove",
    };
})();
