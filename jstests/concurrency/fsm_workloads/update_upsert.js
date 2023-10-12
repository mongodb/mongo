/**
 * update_upsert_multi.js
 *
 * Tests updates that specify upsert=true.
 */

export const $config = (function() {
    let states = {
        update: function update(db, collName) {
            const docId = Random.randInt(5) * 4;
            let updateRes =
                assert.writeOK(db[collName].update({_id: docId}, {$inc: {x: 1}}, {upsert: true}));
            assert.eq(1,
                      updateRes.nMatched + updateRes.nUpserted,
                      "unexpected matched count: " + updateRes);
            assert.eq(1,
                      updateRes.nModified + updateRes.nUpserted,
                      "unexpected modified count: " + updateRes);
        },
    };

    let transitions = {
        update: {update: 1},
    };

    function teardown(db, collName, cluster) {
        assert.eq(0, db[collName].countDocuments({_id: {$nin: [0, 4, 8, 12, 16]}}));
        assert.lt(0, db[collName].countDocuments({_id: {$in: [0, 4, 8, 12, 16]}}));
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
