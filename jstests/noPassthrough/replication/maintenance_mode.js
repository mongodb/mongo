/*
 * Test that mongod accepts the --maintenanceMode parameter and that it works as expected.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

function assertShardingNotEnabled(node) {
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
                                 ErrorCodes.ShardingStateNotInitialized);
}

{
    // --maintenanceMode=replicaSet should auto-initiate a replicaSet but skip auto-bootstrapping
    // a configShard.
    jsTestLog("Testing --maintenanceMode=replicaSet");

    const node = MongoRunner.runMongod({
        maintenanceMode: "replicaSet",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });

    // Node should not be a config server.
    assert.soon(() => node.adminCommand({hello: 1}).isWritablePrimary);
    const config = assert.commandWorked(node.adminCommand({replSetGetConfig: 1})).config;
    assert(!config.hasOwnProperty('configsvr'),
           tojson(config));  // possible race here... cause it fails sometimes

    assertShardingNotEnabled(node);

    // Inserts into replicated collections should succeed.
    assert.commandWorked(node.getDB('test').runCommand({insert: 'testColl', documents: [{x: 1}]}));

    MongoRunner.stopMongod(node);
}

{
    // --maintenanceMode=standalone should disable replicaSet and sharding components.
    jsTestLog("Testing --maintenanceMode=standalone");

    const node = MongoRunner.runMongod({
        maintenanceMode: "standalone",
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });

    // Node should not be part of a replica set.
    const helloResponse = node.getDB("admin").runCommand({hello: 1});
    assert(!helloResponse.hasOwnProperty("setName"));

    assertShardingNotEnabled(node);

    // CRUD operations should succeed.
    assert.commandWorked(node.getDB("test").runCommand({insert: "testColl", documents: [{x: 1}]}));
    const res =
        assert.commandWorked(node.getDB("test").runCommand({count: "testColl", query: {x: 1}}));
    assert.eq(res.n, 1);

    MongoRunner.stopMongod(node);
}

{
    // Nodes started with --maintenanceMode=replicaSet and --replSet should not auto-initiate. The
    // node should be able to be initiated manually and should be able to be added to an existing
    // initiated replicaSet.

    const node0 = MongoRunner.runMongod({
        maintenanceMode: "replicaSet",
        replSet: 'rs',
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        },
    });

    assert.commandFailedWithCode(
        node0.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}),
        ErrorCodes.NotWritablePrimary);

    // An external replSetInitiate should initiate the replica set successfully.
    assert.commandWorked(node0.adminCommand({replSetInitiate: 1}));

    assert.soon(() => node0.adminCommand({hello: 1}).isWritablePrimary);

    assert.commandWorked(
        node0.getDB('testDB').runCommand({insert: 'testColl', documents: [{x: 1}]}));

    // Uninitiated node should succeed in being added to an existing initiated replicaSet.
    const node1 = MongoRunner.runMongod({
        maintenanceMode: "replicaSet",
        replSet: 'rs',
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });

    const config = node0.getDB("local").system.replset.findOne();
    config.members.push({_id: 2, host: node1.host});
    config.version++;
    assert.commandWorked(node0.adminCommand({replSetReconfig: config}));

    assert.soon(() => node1.adminCommand({hello: 1}).secondary);

    MongoRunner.stopMongod(node0);
    MongoRunner.stopMongod(node1);
}

{
    // --maintenanceMode=standalone with --shardsvr, --configsvr, or --replSet should fail.
    const disallowedParameters = ["shardsvr", "configsvr", "replSet"];
    disallowedParameters.forEach(disallowedParameter => {
        jsTestLog(`Testing --maintenanceMode=standalone and --${disallowedParameter}`);
        assert.throws(() => {
            MongoRunner.runMongod({
                maintenanceMode: "standalone",
                [disallowedParameter]: "",
                setParameter: {
                    featureFlagAllMongodsAreSharded: true,
                },
            });
        });
    });
}

{
    // --maintenanceMode=replicaSet with --shardsvr or --configsvr should fail.
    const disallowedParameters = ["shardsvr", "configsvr"];
    disallowedParameters.forEach(disallowedParameter => {
        jsTestLog(`Testing --maintenanceMode=replicaSet and --${disallowedParameter}`);
        assert.throws(() => {
            MongoRunner.runMongod({
                maintenanceMode: "replicaSet",
                [disallowedParameter]: "",
                setParameter: {
                    featureFlagAllMongodsAreSharded: true,
                },
            });
        });
    });
}

{
    // --maintenanceMode should fail.
    jsTestLog("Testing --maintenanceMode");

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
    jsTestLog("Testing --maintenanceMode=nonValidString");

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
