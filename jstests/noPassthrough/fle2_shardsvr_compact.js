/**
 * Cannot run compact against a shard server
 *
 * @tags: [
 * requires_fcv_60,
 * requires_sharding
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

function runTest(mongosConn, shardConn) {
    let dbName = 'testdb';

    let clientMongoS = new EncryptedClient(mongosConn, dbName);

    assert.commandWorked(clientMongoS.createEncryptionCollection("basic", {
        encryptedFields: {
            "fields":
                [{"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}}]
        }
    }));

    let clientShard = new EncryptedClient(shardConn, dbName);

    assert.commandFailedWithCode(clientShard.getDB().basic.compact(), 6583201);
}

jsTestLog("Sharding: Testing fle2 drop collection warning");
{
    const st = new ShardingTest({shards: 1, mongos: 1, config: 1});

    runTest(st.s, st.shard0);

    st.stop();
}
}());
