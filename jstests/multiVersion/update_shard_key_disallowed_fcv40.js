// Test that shard key fields cannot be updated when one or more shards is in FCV 4.0.
// @tags: [uses_transactions, uses_multi_shard_transaction]

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/sharding/libs/update_shard_key_helpers.js");

    let st = new ShardingTest({
        shards: {rs0: {nodes: 3, binVersion: "latest"}, rs1: {nodes: 3, binVersion: "latest"}},
        mongos: 1,
        other: {mongosOptions: {binVersion: "latest"}, configOptions: {binVersion: "latest"}}
    });
    let mongos = st.s0;
    let kDbName = "test";
    let collName = "foo";
    let ns = kDbName + "." + collName;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    function shardCollectionAndMoveChunks(docsToInsert, shardKey, splitDoc, moveDoc) {
        for (let i = 0; i < docsToInsert.length; i++) {
            assert.commandWorked(mongos.getDB(kDbName).foo.insert(docsToInsert[i]));
        }

        assert.commandWorked(mongos.getDB(kDbName).foo.createIndex(shardKey));
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));
        assert.commandWorked(mongos.adminCommand({split: ns, find: splitDoc}));
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: moveDoc, to: st.shard1.shardName}));

        assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));
        st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns});
        st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns});
        st.rs0.getPrimary().adminCommand({_flushDatabaseCacheUpdates: kDbName});
        st.rs1.getPrimary().adminCommand({_flushDatabaseCacheUpdates: kDbName});
    }

    function assertCannotUpdateShardKey(isMixedCluster) {
        // ------------------------------------------------
        // Test changes to shard key run as retryable write
        // ------------------------------------------------
        let session = st.s.startSession({retryWrites: true});
        let sessionDB = session.getDatabase(kDbName);

        // Updates to full shard key
        shardCollectionMoveChunks(
            st, kDbName, ns, {x: 1}, [{x: 30}, {x: 50}, {x: 80}], {x: 50}, {x: 80});
        cleanupOrphanedDocs(st, ns);

        // Assert that updating the shard key when the doc would remain on the same shard fails for
        // both modify and replacement updates
        assert.writeError(sessionDB.foo.update({x: 80}, {$set: {x: 100}}));
        assert.writeError(sessionDB.foo.update({x: 80}, {x: 100}));
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {$set: {x: 100}}});
        });
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {x: 100}});
        });

        // Assert that updating the shard key when the doc would move shards fails for both modify
        // and replacement updates.
        assert.writeError(sessionDB.foo.update({x: 80}, {$set: {x: 3}}));
        assert.commandFailedWithCode(sessionDB.foo.update({x: 80}, {x: 3}),
                                     [ErrorCodes.ImmutableField, ErrorCodes.InvalidOptions]);
        assert.eq(1, mongos.getDB(kDbName).foo.find({x: 80}).itcount());
        assert.eq(0, mongos.getDB(kDbName).foo.find({x: 3}).itcount());

        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {$set: {x: 3}}});
        });
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {x: 3}});
        });

        mongos.getDB(kDbName).foo.drop();

        // Updates to partial shard key
        shardCollectionMoveChunks(st,
                                  kDbName,
                                  ns,
                                  {x: 1, y: 1},
                                  [{x: 30, y: 4}, {x: 50, y: 50}, {x: 80, y: 100}],
                                  {x: 50, y: 50},
                                  {x: 80, y: 100});
        cleanupOrphanedDocs(st, ns);

        // Assert that updating the shard key when the doc would remain on the same shard fails for
        // both modify and replacement updates
        assert.writeError(sessionDB.foo.update({x: 80}, {$set: {x: 100}}));
        assert.writeError(sessionDB.foo.update({x: 80}, {x: 100}));
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {$set: {x: 100}}});
        });
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {x: 100}});
        });

        // Assert that updating the shard key when the doc would move shards fails for both modify
        // and replacement updates
        assert.writeError(sessionDB.foo.update({x: 80}, {$set: {x: 3}}));
        assert.writeError(sessionDB.foo.update({x: 80}, {x: 3}));
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {$set: {x: 3}}});
        });
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {x: 3}});
        });

        mongos.getDB(kDbName).foo.drop();

        // -----------------------------------------------
        // Test changes to shard key run in a transaction
        // -----------------------------------------------
        session = st.s.startSession();
        sessionDB = session.getDatabase(kDbName);

        // Updates to full shard key
        shardCollectionMoveChunks(
            st, kDbName, ns, {x: 1}, [{x: 30}, {x: 50}, {x: 80}], {x: 50}, {x: 80});
        cleanupOrphanedDocs(st, ns);

        // Assert that updating the shard key when the doc would remain on the same shard fails for
        // both modify and replacement updates
        session.startTransaction();
        assert.writeError(sessionDB.foo.update({x: 80}, {$set: {x: 100}}));
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        session.startTransaction();
        assert.throws(function() {
            sessionDB.foo.findAndModify({query: {x: 80}, update: {x: 100}});
        });
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        mongos.getDB(kDbName).foo.drop();

        if (isMixedCluster) {
            // Assert that updating the shard key when the doc would move shards fails on commit
            // because one of the participants is not in FCV 4.2. If the original write is a
            // retryable write, the write will fail when mongos attempts to run commitTransaction.
            // If the original write is part of a transaction, the write itself will complete
            // successfully, but the transaction will fail to commit.

            // Retryable write - updates to full shard key
            session = st.s.startSession({retryWrites: true});
            sessionDB = session.getDatabase(kDbName);

            shardCollectionMoveChunks(
                st, kDbName, ns, {x: 1}, [{x: 30}, {x: 50}, {x: 80}], {x: 50}, {x: 80});
            cleanupOrphanedDocs(st, ns);

            // Doc will move shards
            assert.writeError(sessionDB.foo.update({x: 30}, {$set: {x: 100}}));
            assert.throws(function() {
                sessionDB.foo.findAndModify({query: {x: 30}, update: {$set: {x: 100}}});
            });

            // Doc will remain on the same shard. Because shard 0 is on FCV 4.2, these should
            // complete successfully.
            sessionDB.foo.findAndModify({query: {x: 30}, update: {$set: {x: 5}}});
            st.rs0.awaitReplication();
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 30}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 30}).itcount());

            sessionDB.foo.findAndModify({query: {x: 5}, update: {x: 10}});
            st.rs0.awaitReplication();
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 10}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 10}).itcount());

            assert.commandWorked(sessionDB.foo.update({x: 10}, {$set: {x: 25}}));
            st.rs0.awaitReplication();
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 25}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 25}).itcount());
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 10}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 10}).itcount());

            assert.commandWorked(sessionDB.foo.update({x: 25}, {x: 3}));
            st.rs0.awaitReplication();
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 25}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 25}).itcount());
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 3}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 3}).itcount());

            mongos.getDB(kDbName).foo.drop();

            // Retryable write - updates to partial shard key
            shardCollectionMoveChunks(st,
                                      kDbName,
                                      ns,
                                      {x: 1, y: 1},
                                      [{x: 30, y: 4}, {x: 50, y: 50}, {x: 80, y: 100}],
                                      {x: 50, y: 50},
                                      {x: 80, y: 100});
            cleanupOrphanedDocs(st, ns);

            assert.writeError(sessionDB.foo.update({x: 30}, {$set: {x: 100}}));
            assert.throws(function() {
                sessionDB.foo.findAndModify({query: {x: 30}, update: {x: 100}});
            });

            mongos.getDB(kDbName).foo.drop();

            // Transactional writes
            session = st.s.startSession();
            sessionDB = session.getDatabase(kDbName);

            shardCollectionMoveChunks(
                st, kDbName, ns, {x: 1}, [{x: 10}, {x: 30}, {x: 50}, {x: 80}], {x: 50}, {x: 80});
            cleanupOrphanedDocs(st, ns);

            session.startTransaction();
            assert.commandWorked(sessionDB.foo.update({x: 30}, {$set: {x: 100}}));
            assert.commandFailed(session.commitTransaction_forTesting());

            assert.eq(1, mongos.getDB(kDbName).foo.find({x: 30}).itcount());
            assert.eq(0, mongos.getDB(kDbName).foo.find({x: 100}).itcount());

            session.startTransaction();
            sessionDB.foo.findAndModify({query: {x: 30}, update: {x: 100}});
            assert.commandFailed(session.commitTransaction_forTesting());

            assert.eq(1, mongos.getDB(kDbName).foo.find({x: 30}).itcount());
            assert.eq(0, mongos.getDB(kDbName).foo.find({x: 100}).itcount());

            // Doc will remain on the same shard. Because shard 0 is on FCV 4.2, this should
            // complete successfully.
            session.startTransaction();
            sessionDB.foo.findAndModify({query: {x: 10}, update: {x: 1}});
            assert.commandWorked(sessionDB.foo.update({x: 30}, {$set: {x: 5}}));
            assert.commandWorked(session.commitTransaction_forTesting());
            st.rs0.awaitReplication();

            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 30}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 30}).itcount());
            assert.eq(0, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 10}).itcount());
            assert.eq(0, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 10}).itcount());
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 5}).itcount());
            assert.eq(1, st.rs0.getPrimary().getDB(kDbName).foo.find({x: 1}).itcount());
            assert.eq(1, st.rs0.getSecondaries()[0].getDB(kDbName).foo.find({x: 1}).itcount());

            mongos.getDB(kDbName).foo.drop();
        }
    }

    // Check that updating the shard key fails when all shards are in FCV 4.0
    assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: "4.0"}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), "4.0");
    checkFCV(st.rs0.getPrimary().getDB("admin"), "4.0");
    checkFCV(st.rs1.getPrimary().getDB("admin"), "4.0");

    assertCannotUpdateShardKey(false);

    // Check that updating the shard key fails when shard0 is in FCV 4.2 but shard 1 is in FCV 4.0
    assert.commandWorked(
        st.rs0.getPrimary().getDB("admin").runCommand({setFeatureCompatibilityVersion: "4.2"}));
    checkFCV(st.configRS.getPrimary().getDB("admin"), "4.0");
    checkFCV(st.rs0.getPrimary().getDB("admin"), "4.2");
    checkFCV(st.rs1.getPrimary().getDB("admin"), "4.0");

    assertCannotUpdateShardKey(true);

    st.stop();
})();
