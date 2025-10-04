/**
 * count_odd.js
 *
 * Count odd numbered entries while updating and deleting even numbered entries.
 *
 * @tags: [
 *    requires_non_retryable_writes,
 * ]
 *
 */
export const $config = (function () {
    let states = (function () {
        function init(db, collName) {}

        function write(db, collName) {
            const coll = db[collName];
            const i = Random.randInt(499) * 2;
            retryOnRetryableError(
                () => {
                    assert.writeOK(coll.update({i: i}, {$set: {i: 2000}}, {multi: true}));
                },
                100,
                undefined,
                TestData.runningWithBalancer ? [ErrorCodes.QueryPlanKilled] : [],
            );
            assert.writeOK(coll.remove({i: 2000}));
            assert.writeOK(coll.save({i: i}));
        }

        function count(db, collName) {
            const num_odd_doc = db[collName].countDocuments({i: {$mod: [2, 1]}});
            assert.eq(500, num_odd_doc);
        }

        return {init: init, write: write, count: count};
    })();

    let setup = function (db, collName) {
        assert.commandWorked(db[collName].createIndex({i: 1}));
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < 1000; ++i) {
            bulk.insert({i: i});
        }
        assert.commandWorked(bulk.execute());
    };

    let transitions = {
        init: {write: 0.5, count: 0.5},
        write: {write: 0.5, count: 0.5},
        count: {write: 0.5, count: 0.5},
    };

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        setup: setup,
        transitions: transitions,
    };
})();
