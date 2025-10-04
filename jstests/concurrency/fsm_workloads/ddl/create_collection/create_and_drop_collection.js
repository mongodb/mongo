/**
 * create_and_drop_collection.js
 *
 * Repeatedly creates and drops a collection.
 *
 * @tags: [requires_sharding]
 */
export const $config = (function () {
    let data = {};

    let states = (function () {
        function init(db, collName) {
            this.docNum = 0;
            assert.commandWorked(db[collName].insertOne({_id: this.docNum}));
            checkForDocument(db[collName], this.docNum);
        }

        function checkForDocument(coll, docNum) {
            let docs = coll.find({}).toArray();
            assert.eq(docs.length, 1);
            assert.eq(docs[0]._id, docNum);
        }

        function createShardedCollection(db, collName) {
            assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
            assert.commandWorked(db.adminCommand({shardCollection: db[collName].getFullName(), key: {_id: 1}}));
            assert.commandWorked(db[collName].insertOne({_id: this.docNum}));
            checkForDocument(db[collName], this.docNum);
        }

        function createUnshardedCollection(db, collName) {
            assert.commandWorked(db[collName].insertOne({_id: this.docNum}));
            checkForDocument(db[collName], this.docNum);
        }

        function dropCollection(db, collName) {
            checkForDocument(db[collName], this.docNum++);
            assert(db[collName].drop());
        }

        function dropDatabase(db, collName) {
            checkForDocument(db[collName], this.docNum++);
            assert.commandWorked(db.dropDatabase());
        }

        return {
            init: init,
            createShardedCollection: createShardedCollection,
            createUnshardedCollection: createUnshardedCollection,
            dropCollection: dropCollection,
            dropDatabase: dropDatabase,
        };
    })();

    let transitions = {
        init: {dropCollection: 0.5, dropDatabase: 0.5},
        createShardedCollection: {dropCollection: 0.5, dropDatabase: 0.5},
        createUnshardedCollection: {dropCollection: 0.5, dropDatabase: 0.5},
        dropCollection: {createShardedCollection: 0.5, createUnshardedCollection: 0.5},
        dropDatabase: {createShardedCollection: 0.5, createUnshardedCollection: 0.5},
    };

    // This test in in the concurrency suite because it requires shard stepdowns to properly test
    // that no documents from a newly created collection are dropped from a previous drop
    // collection. There is only one thread because only one collection is being dropped and
    // created.
    return {threadCount: 1, iterations: 50, data: data, states: states, transitions: transitions};
})();
