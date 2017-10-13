/**
 * Checks that issuing a metadata command 1) through mongos or 2) directly against the config server
 * with a non-majority writeConcern fails.
 */
(function() {
    'use strict';

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;
    const newShardName = "newShard";

    const unacceptableWriteConcerns = [
        {writeConcern: {w: 0}},
        {writeConcern: {w: 1}},
        {writeConcern: {w: 2}},
        {writeConcern: {w: 3}},
        // TODO: should metadata commands allow j: false? can CSRS have an in-memory storage engine?
        // writeConcern{w: "majority", j: "false"}},
    ];

    const acceptableWriteConcerns = [
        {},
        {writeConcern: {w: "majority"}},
        {writeConcern: {w: "majority", j: true}},
        {writeConcern: {w: "majority", j: true, wtimeout: 15000}},
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
            assert.commandWorked(st.s.adminCommand({addShard: newShard.name, name: newShardName}));
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
            res = st.s.adminCommand({removeShard: newShardName});
            assert.commandWorked(res);
            assert.eq('completed', res.state);
        },
    };

    function checkCommand(conn, command, setupFunc, cleanupFunc) {
        unacceptableWriteConcerns.forEach(function(writeConcern) {
            jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                       " against " + conn + ", expecting the command to fail");
            setupFunc();
            let commandWithWriteConcern = {};
            Object.assign(commandWithWriteConcern, command, writeConcern);
            assert.commandFailedWithCode(conn.adminCommand(commandWithWriteConcern),
                                         ErrorCodes.InvalidOptions);
            cleanupFunc();
        });

        acceptableWriteConcerns.forEach(function(writeConcern) {
            jsTest.log("testing " + tojson(command) + " with writeConcern " + tojson(writeConcern) +
                       " against " + conn + ", expecting the command to succeed");
            setupFunc();
            let commandWithWriteConcern = {};
            Object.assign(commandWithWriteConcern, command, writeConcern);
            assert.commandWorked(conn.adminCommand(commandWithWriteConcern));
            cleanupFunc();
        });
    }

    var st = new ShardingTest({shards: 1});

    // enableSharding

    checkCommand(st.s, {enableSharding: dbName}, setupFuncs.noop, cleanupFuncs.dropDatabase);

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrEnableSharding: dbName},
                 setupFuncs.noop,
                 cleanupFuncs.dropDatabase);

    // movePrimary

    checkCommand(st.s,
                 {movePrimary: dbName, to: st.shard0.name},
                 setupFuncs.createDatabase,
                 cleanupFuncs.dropDatabase);

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrMovePrimary: dbName, to: st.shard0.name},
                 setupFuncs.createDatabase,
                 cleanupFuncs.dropDatabase);

    // shardCollection

    checkCommand(st.s,
                 {shardCollection: ns, key: {_id: 1}},
                 setupFuncs.enableSharding,
                 cleanupFuncs.dropDatabase);

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrShardCollection: ns, key: {_id: 1}},
                 setupFuncs.enableSharding,
                 cleanupFuncs.dropDatabase);

    // createDatabase

    // Don't check createDatabase against mongos: there is no createDatabase command exposed on
    // mongos; a database is created implicitly when a collection in it is created.

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrCreateDatabase: dbName, to: st.shard0.name},
                 setupFuncs.noop,
                 cleanupFuncs.dropDatabase);

    // addShard

    var newShard = MongoRunner.runMongod({shardsvr: ""});

    checkCommand(st.s,
                 {addShard: newShard.name, name: newShardName},
                 setupFuncs.noop,
                 cleanupFuncs.removeShardIfExists);

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrAddShard: newShard.name, name: newShardName},
                 setupFuncs.noop,
                 cleanupFuncs.removeShardIfExists);

    // removeShard

    checkCommand(st.s, {removeShard: newShardName}, setupFuncs.addShard, cleanupFuncs.noop);

    checkCommand(st.configRS.getPrimary(),
                 {_configsvrRemoveShard: newShardName},
                 setupFuncs.addShard,
                 cleanupFuncs.noop);

    st.stop();
})();
