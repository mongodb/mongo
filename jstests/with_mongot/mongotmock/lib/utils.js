/**
 * Utility functions for mongot tests.
 */

/**
 * @param {Object} expectedCommand The expected mongot $search or $vectorSearch command.
 * @param {DBCollection} coll
 * @param {Mongo} mongotConn
 * @param {NumberLong} cursorId
 * @param {String} searchScoreKey The key could be either '$searchScore' or '$vectorSearchScore'.
 * @returns {Array{Object}} Returns expected results.
 */
export function prepMongotResponse(expectedCommand,
                                   coll,
                                   mongotConn,
                                   cursorId = NumberLong(123),
                                   searchScoreKey = '$vectorSearchScore') {
    const history = [
        {
            expectedCommand,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, ...Object.fromEntries([[searchScoreKey, 0.789]])},
                        {_id: 2, ...Object.fromEntries([[searchScoreKey, 0.654]])},
                        {_id: 4, ...Object.fromEntries([[searchScoreKey, 0.345]])},
                    ]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 11, ...Object.fromEntries([[searchScoreKey, 0.321]])}]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 14, ...Object.fromEntries([[searchScoreKey, 0.123]])}]
                },
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    return [
        {_id: 1, shardKey: 0, x: "ow"},
        {_id: 2, shardKey: 0, x: "now", y: "lorem"},
        {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
        {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    ];
}

/**
 * @param {Mongo} conn
 * @param {String} dbName
 * @param {String} collName
 * @param {Boolean} shouldShard
 */
export function prepCollection(conn, dbName, collName, shouldShard = false) {
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    coll.drop();

    if (shouldShard) {
        // Create and shard the collection so the commands can succeed.
        assert.commandWorked(db.createCollection(collName));
        assert.commandWorked(conn.adminCommand({enableSharding: dbName}));
        assert.commandWorked(
            conn.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}));
    }

    assert.commandWorked(coll.insert([
        // Documents that end up on shard0.
        {_id: 1, shardKey: 0, x: "ow"},
        {_id: 2, shardKey: 0, x: "now", y: "lorem"},
        {_id: 3, shardKey: 0, x: "brown", y: "ipsum"},
        {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
        // Documents that end up on shard1.
        {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"},
        {_id: 13, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    ]));
}
