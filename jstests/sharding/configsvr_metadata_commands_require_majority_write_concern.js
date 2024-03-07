/**
 * Checks that:
 * 1) Issuing a metadata command through a mongos with any write concern succeeds (because we
 * convert it up to majority WC),
 * 2) Issuing a metadata command directly to a config server with non-majority write concern fails.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const newShardName = "newShard";
let newShard;

// Commands sent directly to the config server should fail with WC < majority.
const unacceptableWCsForConfig = [
    {writeConcern: {w: 1}},
    {writeConcern: {w: 2}},
    {writeConcern: {w: 3}},
    // TODO: should metadata commands allow j: false? can CSRS have an in-memory storage engine?
    // writeConcern{w: "majority", j: "false"}},
];

// Only write concern majority can be sent to the config server.
const acceptableWCsForConfig = [
    {writeConcern: {w: "majority"}},
    {writeConcern: {w: "majority", wtimeout: 15000}},
];

// Any write concern can be sent to a mongos, because mongos will upconvert it to majority.
const unacceptableWCsForMongos = [];
const acceptableWCsForMongos = [
    {},
    {writeConcern: {w: 0}},
    {writeConcern: {w: 0, wtimeout: 15000}},
    {writeConcern: {w: 1}},
    {writeConcern: {w: 2}},
    {writeConcern: {w: 3}},
    {writeConcern: {w: "majority"}},
    {writeConcern: {w: "majority", wtimeout: 15000}},
];

const setupFuncs = {
    noop: function() {},
    createDatabase: function() {
        // A database is implicitly created when a collection within it is created.
        assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
    },
    enableSharding: function() {
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    },
    addShard: function() {
        assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
    },
};

const cleanupFuncs = {
    noop: function() {},
    dropDatabase: function() {
        assert.commandWorked(st.s.getDB(dbName).runCommand({dropDatabase: 1}));
    },
    removeShardIfExists: function() {
        var res = st.s.adminCommand({removeShard: newShardName});
        if (!res.ok && res.code == ErrorCodes.ShardNotFound) {
            return;
        }
        assert.commandWorked(res);
        assert.eq('started', res.state);
        removeShard(st, newShardName);
    },
};

function checkCommand(
    conn, command, unacceptableWCs, acceptableWCs, adminCommand, setupFunc, cleanupFunc) {
    unacceptableWCs.forEach(function(writeConcern) {
        jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                   " against " + conn + ", expecting the command to fail");
        setupFunc();
        let commandWithWriteConcern = {};
        Object.assign(commandWithWriteConcern, command, writeConcern);
        if (adminCommand) {
            assert.commandFailedWithCode(conn.adminCommand(commandWithWriteConcern),
                                         ErrorCodes.InvalidOptions);
        } else {
            assert.commandFailedWithCode(conn.runCommand(commandWithWriteConcern),
                                         ErrorCodes.InvalidOptions);
        }
        cleanupFunc();
    });

    acceptableWCs.forEach(function(writeConcern) {
        jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                   " against " + conn + ", expecting the command to succeed");
        setupFunc();
        let commandWithWriteConcern = {};
        Object.assign(commandWithWriteConcern, command, writeConcern);
        if (adminCommand) {
            assert.commandWorked(conn.adminCommand(commandWithWriteConcern));
        } else {
            assert.commandWorked(conn.runCommand(commandWithWriteConcern));
        }
        cleanupFunc();
    });
}

function checkCommandMongos(command, setupFunc, cleanupFunc) {
    checkCommand(st.s,
                 command,
                 unacceptableWCsForMongos,
                 acceptableWCsForMongos,
                 true,
                 setupFunc,
                 cleanupFunc);
}

function checkCommandConfigSvr(command, setupFunc, cleanupFunc) {
    checkCommand(st.configRS.getPrimary(),
                 command,
                 unacceptableWCsForConfig,
                 acceptableWCsForConfig,
                 true,
                 setupFunc,
                 cleanupFunc);
}

let st = new ShardingTest({shards: 1});

// enableSharding
checkCommandMongos({enableSharding: dbName}, setupFuncs.noop, cleanupFuncs.dropDatabase);

// TODO SERVER-77915: remove once 8.0 becomes last-lts
if (FeatureFlagUtil.isPresentAndEnabled(st.s, "TrackUnshardedCollectionsUponCreation")) {
    // changePrimary
    checkCommandMongos({changePrimary: dbName, to: st.shard0.shardName},
                       setupFuncs.createDatabase,
                       cleanupFuncs.dropDatabase);
} else {
    // movePrimary
    checkCommandMongos({movePrimary: dbName, to: st.shard0.shardName},
                       setupFuncs.createDatabase,
                       cleanupFuncs.dropDatabase);
}

// shardCollection
checkCommandMongos(
    {shardCollection: ns, key: {_id: 1}}, setupFuncs.enableSharding, cleanupFuncs.dropDatabase);

// createDatabase
// Don't check createDatabase against mongos: there is no createDatabase command exposed on
// mongos; a database is created implicitly when a collection in it is created.
checkCommandConfigSvr({_configsvrCreateDatabase: dbName, to: st.shard0.shardName},
                      setupFuncs.noop,
                      cleanupFuncs.dropDatabase);

// addShard
newShard = new ReplSetTest({nodes: 1});
newShard.startSet({shardsvr: ''});
newShard.initiate();
checkCommandMongos({addShard: newShard.getURL(), name: newShardName},
                   setupFuncs.noop,
                   cleanupFuncs.removeShardIfExists);
checkCommandConfigSvr({_configsvrAddShard: newShard.getURL(), name: newShardName},
                      setupFuncs.noop,
                      cleanupFuncs.removeShardIfExists);

// removeShard
checkCommandMongos({removeShard: newShardName}, setupFuncs.addShard, cleanupFuncs.noop);
checkCommandConfigSvr(
    {_configsvrRemoveShard: newShardName}, setupFuncs.addShard, cleanupFuncs.noop);

// dropCollection
// We can't use the checkCommandMongos wrapper because it calls adminCommand and dropping admin
// collections are not allowed in mongos.
checkCommand(st.s.getDB(dbName),
             {drop: collName},
             unacceptableWCsForMongos,
             acceptableWCsForMongos,
             false,
             setupFuncs.createDatabase,
             cleanupFuncs.dropDatabase);

// dropDatabase
// We can't use the checkCommandMongos wrapper because we need a connection to the test
// database.
checkCommand(st.s.getDB(dbName),
             {dropDatabase: 1},
             unacceptableWCsForMongos,
             acceptableWCsForMongos,
             false,
             setupFuncs.createDatabase,
             cleanupFuncs.dropDatabase);

newShard.stopSet();
st.stop();
