/**
 * Cannot run cleanup against a shard server
 *
 * @tags: [
 * requires_fcv_71,
 * requires_sharding
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

function runTest(mongosConn, shardConn) {
    let dbName = "testdb";

    let clientMongoS = new EncryptedClient(mongosConn, dbName);

    assert.commandWorked(
        clientMongoS.createEncryptionCollection("basic", {
            encryptedFields: {
                "fields": [{"path": "first", "bsonType": "string", "queries": {"queryType": "equality"}}],
            },
        }),
    );

    let clientShard = new EncryptedClient(shardConn, dbName);

    clientShard.runEncryptionOperation(() => {
        assert.commandFailedWithCode(clientShard.getDB().basic.cleanup(), 7618804);
    });
}

jsTestLog("Sharding: Testing fle2 cleanup not allowed against a shard server");
{
    const st = new ShardingTest({shards: 1, mongos: 1, config: 1});

    runTest(st.s, st.shard0);

    st.stop();
}
