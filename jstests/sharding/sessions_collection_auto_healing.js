/**
 * Requires no shards.
 * @tags: [
 *   config_shard_incompatible,
 *   requires_fcv_70,
 * ]
 */
load('jstests/libs/sessions_collection.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

var st = new ShardingTest({
    shards: 0,
    other: {
        mongosOptions:
            {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}}
    }
});
var configSvr = st.configRS.getPrimary();

var mongos = st.s;
var mongosConfig = mongos.getDB("config");

// Test that we can use sessions on the config server before we add any shards.
{
    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(mongos, false, false);

    assert.commandWorked(configSvr.adminCommand({startSession: 1}));

    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(mongos, false, false);
}

// Test that we can use sessions on a mongos before we add any shards.
{
    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(mongos, false, false);

    assert.commandWorked(mongos.adminCommand({startSession: 1}));

    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(mongos, false, false);
}

// Test that the config server does not create the sessions collection
// if there are not any shards.
{
    assert.eq(mongosConfig.shards.countDocuments({}), 0);

    assert.commandFailedWithCode(configSvr.adminCommand({refreshLogicalSessionCacheNow: 1}),
                                 [ErrorCodes.ShardNotFound]);

    validateSessionsCollection(configSvr, false, false);
}

// Test-wide: add a shard
var rs = new ReplSetTest({nodes: 1});
rs.startSet({shardsvr: ""});
rs.initiate();

var shard = rs.getPrimary();
var shardConfig = shard.getDB("config");

// Test that we can add this shard, even with a local config.system.sessions collection,
// and test that we drop its local collection
{
    shardConfig.system.sessions.insert({"hey": "you"});
    validateSessionsCollection(shard, true, false);

    assert.commandWorked(mongos.adminCommand({addShard: rs.getURL()}));
    assert.eq(mongosConfig.shards.countDocuments({}), 1);
    validateSessionsCollection(shard, false, false);
}

// Test that we can use sessions on a shard before the sessions collection
// is set up by the config servers.
{
    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);

    assert.commandWorked(shard.adminCommand({startSession: 1}));

    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);
}

// Test that we can use sessions from a mongos before the sessions collection
// is set up by the config servers.
{
    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);
    validateSessionsCollection(mongos, false, false);

    assert.commandWorked(mongos.adminCommand({startSession: 1}));

    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);
    validateSessionsCollection(mongos, false, false);
}

// Test that if we do a refresh (write) from a shard server while there
// is no sessions collection, it does not create the sessions collection.
{
    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);

    assert.commandFailedWithCode(shard.adminCommand({refreshLogicalSessionCacheNow: 1}),
                                 [ErrorCodes.NamespaceNotSharded]);

    validateSessionsCollection(configSvr, false, false);
    validateSessionsCollection(shard, false, false);
}

// Test that a refresh on the config servers once there are shards creates
// the sessions collection on a shard.
{
    validateSessionsCollection(shard, false, false);

    assert.commandWorked(configSvr.adminCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(shard, true, true);

    const sessionsOpenedByAddShardCmd = 1;
    const sessionsOpenedByShardCollectionCmd = 2;
    const sessionsOpenedByDDLOps = sessionsOpenedByAddShardCmd + sessionsOpenedByShardCollectionCmd;

    // We will have sessionsOpenedByDDLOps sessions because of the sessions used in the
    // shardCollection's retryable write to shard the sessions collection. It will disappear after
    // we run the refresh function on the shard.
    assert.eq(shardConfig.system.sessions.countDocuments({}),
              sessionsOpenedByDDLOps,
              "did not flush config's sessions");

    // Now, if we do refreshes on the other servers, their in-mem records will
    // be written to the collection.
    assert.commandWorked(shard.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(shardConfig.system.sessions.countDocuments({}),
              sessionsOpenedByDDLOps + 1,
              "did not flush shard's sessions");

    rs.awaitLastOpCommitted();
    assert.commandWorked(mongos.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(shardConfig.system.sessions.countDocuments({}),
              sessionsOpenedByDDLOps + 3,
              "did not flush mongos' sessions");
}

// Test that if we drop the index on the sessions collection, only a refresh on the config
// server heals it.
{
    assert.commandWorked(shardConfig.system.sessions.dropIndex({lastUse: 1}));

    validateSessionsCollection(shard, true, false);

    assert.commandWorked(configSvr.adminCommand({refreshLogicalSessionCacheNow: 1}));
    validateSessionsCollection(shard, true, true);

    assert.commandWorked(shardConfig.system.sessions.dropIndex({lastUse: 1}));

    assert.commandWorked(shard.adminCommand({refreshLogicalSessionCacheNow: 1}));
    validateSessionsCollection(shard, true, false);
}

st.stop();
rs.stopSet();
})();
