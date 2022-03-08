'use strict';

/**
 * update_upsert_multi.js
 *
 * Tests updates that specify upsert=true.
 *
 * @tags: [__TEMPORARILY_DISABLED__]
 */
let $config = (function() {
    let states = {
        update: function update(db, collName) {
            const docId = Random.randInt(5) * 4;
            let updateRes =
                assert.writeOK(db[collName].update({_id: docId}, {$inc: {x: 1}}, {upsert: true}));
            assertAlways.eq(1, updateRes.nModified);
            assertAlways.eq(1, updateRes.nMatched + updateRes.nUpserted);
        },
    };

    let transitions = {
        update: {update: 1},
    };

    function teardown(db, collName, cluster) {
        assertAlways.eq(0, db[collName].countDocuments({_id: {$nin: [0, 4, 8, 12, 16]}}));
        assertAlways.lt(0, db[collName].countDocuments({_id: {$in: [0, 4, 8, 12, 16]}}));
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        startState: 'update',
        transitions: transitions,
        teardown: teardown,
    };
})();
