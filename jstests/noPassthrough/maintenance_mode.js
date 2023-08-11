/*
 * Test that mongod accepts the --maintenanceMode parameter and that it works as expected.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

{
    // --maintenanceMode=replicaSet should auto-initiate a replicaSet but skip auto-bootstrapping a
    // configShard.
    const node = MongoRunner.runMongod({
        maintenanceMode: "replicaSet",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });

    // Node should not be a config server.
    assert.soon(() => node.adminCommand({hello: 1}).isWritablePrimary);
    const config = assert.commandWorked(node.adminCommand({replSetGetConfig: 1})).config;
    assert(!config.hasOwnProperty('configsvr'), tojson(config));

    // Sharding should not be enabled.
    let findCmd = {
        find: 'user',
        filter: {_id: 9876},
        // Attaching databaseVersion tells the server we are running a sharding command.
        databaseVersion: {
            uuid: new UUID("ebee4d32-ef8b-40cb-b75e-b45fbe4042dc"),
            timestamp: new Timestamp(1691525961, 12),
            lastMod: NumberInt(5),
        }
    };
    assert.commandFailedWithCode(node.getDB('test').runCommand(findCmd),
                                 ErrorCodes.NoShardingEnabled);

    // Inserts into replicated collections should succeed.
    assert.commandWorked(node.getDB('test').runCommand({insert: 'testColl', documents: [{x: 1}]}));

    MongoRunner.stopMongod(node);
}

{
    // --maintenanceMode=standalone should succeed.
    const node = MongoRunner.runMongod({
        maintenanceMode: "standalone",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });
    MongoRunner.stopMongod(node);
}

{
    // --maintenanceMode should fail.
    assert.throws(() => {
        MongoRunner.runMongod({
            maintenanceMode: "",
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            },
        });
    });
}

{
    // --maintenanceMode=nonValidString should fail.
    assert.throws(() => {
        MongoRunner.runMongod({
            maintenanceMode: "nonValidString",
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            },
        });
    });
}
})();
