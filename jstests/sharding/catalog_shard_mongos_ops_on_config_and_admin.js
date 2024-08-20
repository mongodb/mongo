/**
 * Tests to make sure that the mongos does not allow certain commands on the config and admin
 * databases when configShard is enabled.
 *
 * @tags: [requires_fcv_80]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({mongos: 1, shards: 1, config: 1, configShard: true});
// TODO (SERVER-88675): DDL commands against config and admin database are not allowed via a router
// but are allowed via a direct connection to the config server or shard.
const isReplicaSetEndpointActive = st.isReplicaSetEndpointActive();

let mongosAdminDB = st.s0.getDB("admin");
let mongosConfigDB = st.s0.getDB("config");
let configSvrAdminDB = st.config0.getDB("admin");
let configSvrConfigDB = st.config0.getDB("config");

// Create a role so that the admin.system.roles collection exists for the tests
var cmdObj = {createRole: "customRole", roles: [], privileges: []};
var res = mongosAdminDB.runCommand(cmdObj);
assert.commandWorked(res);

// Commands that should fail when run on collections in the config database when
// configShard is enabled
{
    assert.commandFailedWithCode(
        mongosAdminDB.runCommand({renameCollection: "config.shards", to: "config.joe"}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(mongosConfigDB.runCommand({drop: "shards"}),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(
        mongosConfigDB.runCommand(
            {aggregate: "shards", pipeline: [{$out: "shards"}], cursor: {}, writeConcern: {w: 1}}),
        31321);

    assert.commandFailedWithCode(
        mongosAdminDB.adminCommand(
            {reshardCollection: "config.system.sessions", key: {uid: 1}, numInitialChunks: 2}),
        ErrorCodes.IllegalOperation);
}

// Commands that should fail when run on collections in the admin database when
// configShard is enabled
{
    assert.commandFailedWithCode(
        mongosAdminDB.runCommand({renameCollection: "admin.system.roles", to: "admin.joe"}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(mongosAdminDB.runCommand({drop: "system.healthLog"}),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(mongosAdminDB.runCommand({
        aggregate: "system.roles",
        pipeline: [{$out: "system.roles"}],
        cursor: {},
        writeConcern: {w: 1}
    }),
                                 17385);

    assert.commandFailedWithCode(
        mongosAdminDB.adminCommand(
            {reshardCollection: "admin.system.roles", key: {uid: 1}, numInitialChunks: 2}),
        [ErrorCodes.NamespaceNotFound, ErrorCodes.NamespaceNotSharded]);
}

// Mongos commands on the config database that previously failed that should still fail when run
// directly on the config server
{
    assert.commandFailedWithCode(
        configSvrConfigDB.runCommand(
            {aggregate: "shards", pipeline: [{$out: "shards"}], cursor: {}, writeConcern: {w: 1}}),
        31321);
}

// Mongos commands on the config database that previoulsy failed that should succeed when run
// directly on the config server
{
    const renameRes0 =
        configSvrAdminDB.runCommand({renameCollection: "config.shards", to: "config.joe"});
    const renameRes1 =
        configSvrAdminDB.runCommand({renameCollection: "config.joe", to: "config.shards"});

    if (isReplicaSetEndpointActive) {
        assert.commandFailedWithCode(renameRes0, ErrorCodes.IllegalOperation);
        assert.commandFailedWithCode(renameRes1, ErrorCodes.IllegalOperation);
    } else {
        assert.commandWorked(renameRes0);
        assert.commandWorked(renameRes1);
    }

    const dropRes = configSvrConfigDB.runCommand({drop: "shards"});
    if (isReplicaSetEndpointActive) {
        assert.commandFailedWithCode(dropRes, ErrorCodes.IllegalOperation);
    } else {
        assert.commandWorked(dropRes);
    }
}

// Mongos commands on the admin database that previously failed that should still fail when run
// directly on the config server
{
    assert.commandFailedWithCode(configSvrAdminDB.runCommand({
        aggregate: "system.roles",
        pipeline: [{$out: "system.roles"}],
        cursor: {},
        writeConcern: {w: 1}
    }),
                                 17385);
}
// Mongos commands on the admin database that previously failed that should succeed when run
// directly on the config server.
// Note: renameCollection and drop will still fail for certain collections in the admin databases.
{
    const renameRes0 =
        configSvrAdminDB.runCommand({renameCollection: "admin.system.roles", to: "admin.joe"});
    if (isReplicaSetEndpointActive) {
        assert.commandFailedWithCode(renameRes0, ErrorCodes.IllegalOperation);
    } else {
        assert.commandWorked(renameRes0);
    }

    const renameRes1 =
        configSvrAdminDB.runCommand({renameCollection: "admin.joe", to: "admin.system.roles"});
    if (isReplicaSetEndpointActive) {
        assert.commandFailedWithCode(renameRes1, ErrorCodes.IllegalOperation);
    } else {
        assert.commandWorked(renameRes1);
    }

    const dropRes = configSvrConfigDB.runCommand({drop: "system.healthLog"});
    if (isReplicaSetEndpointActive) {
        assert.commandFailedWithCode(dropRes, ErrorCodes.IllegalOperation);
    } else {
        assert.commandWorked(dropRes);
    }
}

// TODO SERVER-74570: Enable parallel shutdown
st.stop({parallelSupported: false});
