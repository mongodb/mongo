/**
 * update_and_batched_delete.js
 *
 * Concurrently run batched deletes and updates which can move the documents physically.
 * Adapted from Justin's repro in SERVER-95570.
 *
 * @tags: [
 *   # limit: 0 deletes.
 *   requires_non_retryable_writes,
 * ]
 */

const timeToLiveSeconds = 1;
const maxDocuments = 10; // Thanks, Max, for lending us these documents.

export const $config = (function () {
    function setup(db, collName, cluster) {
        assert.commandWorked(db[collName].createIndex({updateTime: 1}));
    }

    const states = {
        upsert: function upsert(db, collName) {
            const now = ISODate();
            const documentId = Math.floor(Random.rand() * maxDocuments);

            assert.commandWorked(
                db[collName].update(
                    {_id: documentId},
                    {$set: {updateTime: new Date(now - 10000)}, $setOnInsert: {creationTime: now}},
                    {upsert: true},
                ),
            );
        },
        delete: function doDelete(db, collName) {
            assert.commandWorked(
                db[collName].deleteMany({updateTime: {$lt: new Date(Date.now() - timeToLiveSeconds * 1000)}}),
            );
        },
    };

    return {
        threadCount: 12,
        iterations: 3000,
        startState: "upsert",
        states: states,
        setup: setup,
        transitions: {upsert: {upsert: 1.0, delete: 1.0}, delete: {upsert: 1.0, delete: 1.0}},
    };
})();
