(function() {
    'use strict';

    var conn = MongoRunner.runMongod();

    var res;

    var connA = new Mongo(conn.host);
    res = assert.commandFailed(connA.adminCommand({
        moveChunk: 'DummyDB.DummyColl',
        configdb: 'localhost:1',
        fromShard: 'DummyFromShard',
        toShard: 'DummyToShard',
        min: {Key: -1},
        max: {Key: 1},
        maxChunkSizeBytes: 1024,
        maxTimeMS: 10000
    }));
    assert.eq(ErrorCodes.ExceededTimeLimit, res.code);

    // Sharding state is now initialized at this point, although with an unreachable config.

    var connB = new Mongo(conn.host);
    res = assert.commandFailed(connB.adminCommand({
        moveChunk: 'DummyDB.DummyColl',
        configdb: 'localhost:1',
        fromShard: 'DummyFromShard',
        toShard: 'DummyToShard',
        min: {Key: -1},
        max: {Key: 1},
        maxChunkSizeBytes: 1024,
        maxTimeMS: 10000
    }));
    // Cannot reach config server.
    assert.eq(ErrorCodes.HostUnreachable, res.code);

    MongoRunner.stopMongod(conn);

})();
