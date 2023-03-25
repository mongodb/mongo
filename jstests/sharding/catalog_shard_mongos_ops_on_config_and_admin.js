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

// Create a role so that the admin.system.roles collection exists for the tests
var cmdObj = {createRole: "customRole", roles: [], privileges: []};
var res = mongosAdminDB.runCommand(cmdObj);
assert.commandWorked(res);

// Ensure that the mongos does not allow renameCollection, drop and $out (from an aggregation
// pipeline) on collections in the config database when catalogShard is enabled.
{
    assert.commandFailedWithCode(
        mongosAdminDB.runCommand({renameCollection: "config.shards", to: "config.joe"}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(mongosConfigDB.runCommand({drop: "shards"}),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailed(mongosConfigDB.runCommand(
        {aggregate: "shards", pipeline: [{$out: "shards"}], cursor: {}, writeConcern: {w: 1}}));
}

// Ensure that the mongos does not allow renameCollection, drop and $out (from an aggregation
// pipeline) on collections in the admin database when catalogShard is enabled.
{
    assert.commandFailedWithCode(
        mongosAdminDB.runCommand({renameCollection: "admin.system.roles", to: "admin.joe"}),
        ErrorCodes.IllegalOperation);

    assert.commandFailedWithCode(mongosAdminDB.runCommand({drop: "system.healthLog"}),
                                 ErrorCodes.IllegalOperation);

    assert.commandFailed(mongosAdminDB.runCommand({
        aggregate: "system.roles",
        pipeline: [{$out: "system.roles"}],
        cursor: {},
        writeConcern: {w: 1}
    }));
}

let configSvrAdminDB = st.config0.getDB("admin");
let configSvrConfigDB = st.config0.getDB("config");
// Ensure that renameCollection and drop are still allowed on the config database when run directly
// on the config server, and that $out fails when run directly on the config server.
{
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "config.shards", to: "config.joe"}));
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "config.joe", to: "config.shards"}));

    assert.commandFailed(configSvrConfigDB.runCommand(
        {aggregate: "shards", pipeline: [{$out: "shards"}], cursor: {}, writeConcern: {w: 1}}));

    assert.commandWorked(configSvrConfigDB.runCommand({drop: "shards"}));
}

// Ensure that renameCollection and drop are still allowed on the admin database when run
// directly on the config server, and that $out fails when run directly on the config server.
// Note: renameCollection and drop will still fail for certain collections in the admin databases.
{
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "admin.system.roles", to: "admin.joe"}));
    assert.commandWorked(
        configSvrAdminDB.runCommand({renameCollection: "admin.joe", to: "admin.system.roles"}));

    assert.commandFailed(configSvrAdminDB.runCommand({
        aggregate: "system.roles",
        pipeline: [{$out: "system.roles"}],
        cursor: {},
        writeConcern: {w: 1}
    }));

    assert.commandWorked(configSvrAdminDB.runCommand({drop: "system.healthLog"}));
}

// TODO SERVER-74570: Enable parallel shutdown
st.stop({parallelSupported: false});
})();
