/**
 * update_and_bulk_insert.js
 *
 * Each thread alternates between inserting 100 documents and updating every document in the
 * collection.
 *
 * This workload was designed to test for an issue similar to SERVER-20512 with UpdateStage, where
 * we attempted to make a copy of a record after a WriteConflictException occurred in
 * Collection::updateDocument().
 *
 * @tags: [
 *   # Runs a multi-update which is non-retryable.
 *   requires_non_retryable_writes
 * ]
 *
 */
export const $config = (function () {
    let states = {
        insert: function insert(db, collName) {
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < 10; ++i) {
                bulk.insert({});
            }
            assert.commandWorked(bulk.execute());
        },

        update: function update(db, collName) {
            let res = db[collName].update({}, {$inc: {n: 1}}, {multi: true});
            assert.lte(0, res.nMatched, tojson(res));
            assert.eq(res.nMatched, res.nModified, tojson(res));
            assert.eq(0, res.nUpserted, tojson(res));
        },
    };

    let transitions = {insert: {insert: 0.2, update: 0.8}, update: {insert: 0.2, update: 0.8}};

    return {
        threadCount: 5,
        iterations: 30,
        startState: "insert",
        states: states,
        transitions: transitions,
    };
})();
