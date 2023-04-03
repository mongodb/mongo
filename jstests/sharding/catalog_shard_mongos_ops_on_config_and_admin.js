/**
 * Tests to make sure that the mongos does not allow certain commands on the config and admin
 * databases when catalogShard is enabled.
 *
 * @tags: [requires_fcv_70, featureFlagCatalogShard, featureFlagTransitionToCatalogShard]
 */
(function() {
"use strict";

var st = new ShardingTest({mongos: 1, shards: 1, config: 1, catalogShard: true});

let mongosAdminDB = st.s0.getDB("admin");
let mongosConfigDB = st.s0.getDB("config");
let configSvrAdminDB = st.config0.getDB("admin");
let configSvrConfigDB = st.config0.getDB("config");

// Create a role so that the admin.system.roles collection exists for the tests
var cmdObj = {createRole: "customRole", roles: [], privileges: []};
var res = mongosAdminDB.runCommand(cmdObj);
assert.commandWorked(res);

// Commands that should fail when run on collections in the config database when
// catalogShard is enabled
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
// catalogShard is enabled
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
        ErrorCodes.NamespaceNotSharded);
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
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "config.shards", to: "config.joe"}));

    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "config.joe", to: "config.shards"}));

    assert.commandWorked(configSvrConfigDB.runCommand({drop: "shards"}));
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
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "admin.system.roles", to: "admin.joe"}));

    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "admin.joe", to: "admin.system.roles"}));

    assert.commandWorked(configSvrAdminDB.runCommand({drop: "system.healthLog"}));
}

// TODO SERVER-74570: Enable parallel shutdown
st.stop({parallelSupported: false});
})();
