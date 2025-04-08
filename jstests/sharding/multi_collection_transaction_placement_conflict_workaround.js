/*
 * Tests that multi-document transactions fail with MigrationConflict/SnapshotUnavailable (and
 * TransientTransactionError label) if a collection or database placement changes have occurred
 * later than the transaction data snapshot timestamp.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});

// Test transaction with concurrent chunk migration.
{
    const dbName = 'test_txn_with_chunk_migration';
    const collName1 = 'coll1';
    const collName2 = 'coll2';
    const ns1 = dbName + '.' + collName1;
    const ns2 = dbName + '.' + collName2;

    let coll1 = st.s.getDB(dbName)[collName1];
    let coll2 = st.s.getDB(dbName)[collName2];
    // Setup initial state:
    //   ns1: unsharded collection on shard0, with documents: {a: 0}
    //   ns2: sharded collection with chunks both on shard0 and shard1, with documents: {x: -1}, {x:
    //   1}
    st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

    st.adminCommand({shardCollection: ns2, key: {x: 1}});
    assert.commandWorked(st.splitAt(ns2, {x: 0}));
    assert.commandWorked(st.moveChunk(ns2, {x: -1}, st.shard0.shardName));
    assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard1.shardName));

    assert.commandWorked(coll1.insert({a: 1}));

    assert.commandWorked(coll2.insert({x: -1}));
    assert.commandWorked(coll2.insert({x: 1}));

    // Start a multi-document transaction and make one read on shard0
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl1 = sessionDB.getCollection(collName1);
    const sessionColl2 = sessionDB.getCollection(collName2);
    session.startTransaction();  // Default is local RC. With snapshot RC there's no bug.
    assert.eq(1, sessionColl1.find().itcount());

    // While the transaction is still open, move ns2's [0, 100) chunk to shard0.
    assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard0.shardName));
    // Refresh the router so that it doesn't send a stale SV to the shard, which would cause the txn
    // to be aborted.
    assert.eq(2, coll2.find().itcount());

    // Trying to read coll2 will result in an error. Note that this is not retryable even with
    // enableStaleVersionAndSnapshotRetriesWithinTransactions enabled because the first statement
    // always had an active snapshot open on the same shard this request is trying to contact.
    let err = assert.throwsWithCode(() => {
        sessionColl2.find().itcount();
    }, ErrorCodes.MigrationConflict);

    assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

    // Transaction abort can race 'abort' before 'new transaction' command on participants, so
    // ensure there are no orphan transactions on participant.
    for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
        assert.commandWorked(db.runCommand({killSessions: [session.id]}));
    }
}

// Test transaction with concurrent move primary.
{
    function runTest(readConcernLevel) {
        const dbName1 = 'test_txn_with_move_primary_and_' + readConcernLevel;
        const dbName2 = 'test_txn_with_move_primary_and_' + readConcernLevel + '_2';
        const collName1 = 'coll1';
        const collName2 = 'coll2';

        st.adminCommand({enableSharding: dbName1, primaryShard: st.shard0.shardName});
        st.adminCommand({enableSharding: dbName2, primaryShard: st.shard1.shardName});

        const coll1 = st.getDB(dbName1)[collName1];
        coll1.insert({x: 1, c: 0});

        const coll2 = st.getDB(dbName2)[collName2];
        coll2.insert({x: 2, c: 0});

        // Start a multi-document transaction. Execute one statement that will target shard0.
        let session = st.s.startSession();
        session.startTransaction({readConcern: {level: readConcernLevel}});
        assert.eq(1, session.getDatabase(dbName1)[collName1].find().itcount());

        // Run movePrimary to move dbName2 from shard1 to shard0.
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName2, to: st.shard0.shardName}));

        // Make sure the router has fresh routing info to avoid causing the transaction to fail due
        // to StaleConfig.
        assert.eq(1, coll2.find().itcount());

        // Execute a second statement, now on dbName2. This statement will be routed to shard0
        // (since there's no historical routing for databases). Expect it to fail with
        // MigrationConflict error.
        let err = assert.throwsWithCode(() => {
            session.getDatabase(dbName2)[collName2].find().itcount();
        }, ErrorCodes.MigrationConflict);

        assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

        // Transaction abort can race 'abort' before 'new transaction' command on participants, so
        // ensure there are no orphan transactions on participant.
        for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
            assert.commandWorked(db.runCommand({killSessions: [session.id]}));
        }
    }

    // These tests only make sense with untracked collections since movePrimary does not affect
    // tracked collections
    const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
    if (!isTrackUnshardedUponCreationEnabled) {
        runTest('majority');

        runTest('snapshot');
    }
}

// Test transaction with concurrent move collection.
{
    function runTest(readConcernLevel) {
        const dbName1 = 'test_move_collection_' + readConcernLevel;
        const dbName2 = 'test_move_collection_' + readConcernLevel + '_2';
        const collName1 = 'coll1';
        const collName2 = 'coll2';
        const ns2 = dbName2 + '.' + collName2;

        st.adminCommand({enableSharding: dbName1, primaryShard: st.shard0.shardName});
        st.adminCommand({enableSharding: dbName2, primaryShard: st.shard1.shardName});

        const coll1 = st.getDB(dbName1)[collName1];
        coll1.insert({x: 1, c: 0});

        const coll2 = st.getDB(dbName2)[collName2];
        coll2.insert({x: 2, c: 0});

        // Start a multi-document transaction. Execute one statement that will target shard0.
        let session = st.s.startSession();
        session.startTransaction({readConcern: {level: readConcernLevel}});
        assert.eq(1, session.getDatabase(dbName1)[collName1].find().itcount());

        // Run moveCollection to move dbName2.collName2 from shard1 to shard0.
        assert.commandWorked(
            st.s.adminCommand({moveCollection: ns2, toShard: st.shard0.shardName}));

        // Make sure the router has fresh routing info to avoid causing the transaction to fail due
        // to StaleConfig.
        assert.eq(1, coll2.find().itcount());

        // Execute a second statement, now on dbName2. With majority read concern, this is
        // equivalent to the moveChunk scenario. However, since resharding changes the uuid, in the
        // case of snapshot read concern, the router will fail to find chunk history for the
        // timestamp before even sending the request to the shard.
        const expectedError = readConcernLevel == 'snapshot' ? ErrorCodes.StaleChunkHistory
                                                             : ErrorCodes.MigrationConflict;
        let err = assert.throwsWithCode(() => {
            session.getDatabase(dbName2)[collName2].find().itcount();
        }, expectedError);

        assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

        // Transaction abort can race 'abort' before 'new transaction' command on participants, so
        // ensure there are no orphan transactions on participant.
        for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
            assert.commandWorked(db.runCommand({killSessions: [session.id]}));
        }
    }

    // These tests only make sense with tracked, unsharded collections since moveCollection does not
    // affect untracked collections
    const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
    if (isTrackUnshardedUponCreationEnabled) {
        runTest('majority');

        runTest('snapshot');
    }
}

// Tests transactions with concurrent DDL operations.
{
    const readConcerns = ['local', 'snapshot'];
    const commands = ['find', 'aggregate', 'update'];

    // Test transaction involving sharded collection with concurrent rename, where the transaction
    // attempts to read the renamed-to collection.
    {
        function runTest(readConcernLevel, command) {
            const dbName = 'test_rename_with_' + command + '_and_' + readConcernLevel;
            const collName1 = 'coll1';
            const collName2 = 'coll2';
            const collName3 = 'coll3';
            const ns1 = dbName + '.' + collName1;

            let coll1 = st.s.getDB(dbName)[collName1];
            let coll2 = st.s.getDB(dbName)[collName2];
            let coll3 = st.s.getDB(dbName)[collName3];

            jsTest.log("Running transaction + rename test with read concern " + readConcernLevel +
                       " and command " + command);

            // 1. Initial state:
            //   ns1: sharded collection with chunks both on shard0 and shard1, with documents: {x:
            //   -1}, {x: 1}, one doc on each shard. ns2: unsharded collection on shard0, with
            //   documents: {a: 0}. ns3: does not exist.
            // 2. Start txn, hit shard0 for ns2 [shard0's snapshot has: ns1 and ns2]
            // 3. Rename ns1 -> ns3
            // 4. Target ns3. On shard0, ns3 does not exist on the txn snapshot. On shard1 it will.
            //    Transaction should conflict, otherwise the txn would see half the collection.

            // Setup initial state:
            st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

            st.adminCommand({shardCollection: ns1, key: {x: 1}});
            assert.commandWorked(st.splitAt(ns1, {x: 0}));
            assert.commandWorked(st.moveChunk(ns1, {x: -1}, st.shard0.shardName));
            assert.commandWorked(st.moveChunk(ns1, {x: 0}, st.shard1.shardName));

            assert.commandWorked(coll1.insert({x: -1}));
            assert.commandWorked(coll1.insert({x: 1}));

            assert.commandWorked(coll2.insert({a: 1}));

            // Start a multi-document transaction and make one read on shard0
            const session = st.s.startSession();
            const sessionDB = session.getDatabase(dbName);
            const sessionColl2 = sessionDB.getCollection(collName2);
            const sessionColl3 = sessionDB.getCollection(collName3);
            session.startTransaction({readConcern: {level: readConcernLevel}});
            assert.eq(1, sessionColl2.find().itcount());  // Targets shard0.

            // While the transaction is still open, rename coll1 to coll3.
            assert.commandWorked(coll1.renameCollection(collName3));

            // Refresh the router so that it doesn't send a stale SV to the shard, which would cause
            // the txn to be aborted.
            assert.eq(2, coll3.find().itcount());

            // Now read coll3 within the transaction and expect to get a conflict.
            let err = assert.throwsWithCode(() => {
                if (command === 'find') {
                    sessionColl3.find().itcount();
                } else if (command === 'aggregate') {
                    sessionColl3.aggregate().itcount();
                } else if (command === 'update') {
                    assert.commandWorked(sessionColl3.update({x: 1}, {$set: {c: 1}}));
                }
            }, [ErrorCodes.SnapshotUnavailable]);
            assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

            // Transaction abort can race 'abort' before 'new transaction' command on participants,
            // so ensure there are no orphan transactions on participant.
            for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
                assert.commandWorked(db.runCommand({killSessions: [session.id]}));
            }
        }

        readConcerns.forEach((readConcern) => commands.forEach((command) => {
            runTest(readConcern, command);
        }));
    }

    // Test transaction involving sharded collection with concurrent drop, where the transaction
    // attempts to read the dropped collection.
    {
        function runTest(readConcernLevel, command) {
            const dbName = 'test_txn_and_drop_with_' + command + '_and_' + readConcernLevel;
            const collName1 = 'coll1';
            const collName2 = 'coll2';
            const ns1 = dbName + '.' + collName1;

            let coll1 = st.s.getDB(dbName)[collName1];
            let coll2 = st.s.getDB(dbName)[collName2];

            jsTest.log("Running transaction + drop test with read concern " + readConcernLevel +
                       " and command " + command);
            assert(command === 'find' || command === 'aggregate' || command === 'update');
            // Initial state:
            //    shard0 (dbPrimary): collA(sharded) and collB(unsharded)
            //    shard1: collA(sharded)
            //
            // 1. Start txn, hit shard0 for collB
            // 2. Drop collA
            // 3. Read collA. Will target only shard0 because the router believes it is no longer
            //    sharded, so it would read the sharded coll (but just half of it). Therefore, a
            //    conflict must be raised.

            // Setup initial state:
            st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

            st.adminCommand({shardCollection: ns1, key: {x: 1}});
            assert.commandWorked(st.splitAt(ns1, {x: 0}));
            assert.commandWorked(st.moveChunk(ns1, {x: -1}, st.shard0.shardName));
            assert.commandWorked(st.moveChunk(ns1, {x: 0}, st.shard1.shardName));

            assert.commandWorked(coll1.insert({x: -1}));
            assert.commandWorked(coll1.insert({x: 1}));

            assert.commandWorked(coll2.insert({a: 1}));

            // Start a multi-document transaction and make one read on shard0 for ns2/
            const session = st.s.startSession();
            const sessionDB = session.getDatabase(dbName);
            const sessionColl1 = sessionDB.getCollection(collName1);
            const sessionColl2 = sessionDB.getCollection(collName2);
            session.startTransaction({readConcern: {level: readConcernLevel}});
            assert.eq(1, sessionColl2.find().itcount());  // Targets shard0.

            // While the transaction is still open, drop coll1.
            assert(coll1.drop());

            // Refresh the router so that it doesn't send a stale SV to the shard, which would cause
            // the txn to be aborted.
            assert.eq(0, coll1.find().itcount());

            // Now read coll1 within the transaction and expect to get a conflict.
            let isWriteCommand = command === 'update';
            let err = assert.throwsWithCode(() => {
                if (command === 'find') {
                    sessionColl1.find().itcount();
                } else if (command === 'aggregate') {
                    sessionColl1.aggregate().itcount();
                } else if (command === 'update') {
                    assert.commandWorked(sessionColl1.update({x: 1}, {$set: {c: 1}}));
                }
            }, isWriteCommand ? ErrorCodes.WriteConflict : ErrorCodes.SnapshotUnavailable);
            assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

            // Transaction abort can race 'abort' before 'new transaction' command on participants,
            // so ensure there are no orphan transactions on participant.
            for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
                assert.commandWorked(db.runCommand({killSessions: [session.id]}));
            }
        }

        readConcerns.forEach((readConcern) => commands.forEach((command) => {
            runTest(readConcern, command);
        }));
    }

    // Test transaction concurrent with reshardCollection.
    {
        function runTest(readConcernLevel, command) {
            const dbName =
                'test_txn_and_reshardCollection_with_' + command + '_and_' + readConcernLevel;
            const collName1 = 'coll1';
            const collName2 = 'coll2';
            const ns1 = dbName + '.' + collName1;

            let coll1 = st.s.getDB(dbName)[collName1];
            let coll2 = st.s.getDB(dbName)[collName2];

            jsTest.log("Running transaction + resharding test with read concern " +
                       readConcernLevel + " and command " + command);

            // Setup initial state:
            st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

            st.adminCommand({shardCollection: ns1, key: {x: 1}});
            assert.commandWorked(st.splitAt(ns1, {x: 0}));
            assert.commandWorked(st.moveChunk(ns1, {x: -1}, st.shard0.shardName));
            assert.commandWorked(st.moveChunk(ns1, {x: 1}, st.shard1.shardName));

            assert.commandWorked(coll1.insertMany([{x: -1, y: 0}, {x: 1, y: 0}]));

            assert.commandWorked(coll2.insertOne({a: 1}));

            // Set fp to block resharding after commit on configsvr but before commit on shards.
            // We seek to test an interleaving where the transaction executes at a logical timestamp
            // that falls between the timestamp at which resharding commits on the configsvr and the
            // timestamp at which the temporary resharding collections are renamed to the original
            // nss on the shards.
            const fp = configureFailPoint(st.configRS.getPrimary(),
                                          "reshardingPauseBeforeTellingParticipantsToCommit");

            // On parallel shell, start resharding
            const joinResharding =
                startParallelShell(funWithArgs((nss) => {
                                       assert.commandWorked(db.adminCommand({
                                           reshardCollection: nss,
                                           key: {y: 1},
                                           // We set this to 2 because we want there to be one chunk
                                           // on each shard post resharding for tests below.
                                           numInitialChunks: 2,
                                       }));
                                   }, ns1), st.s.port);

            // Await configsvr to have done its part of the commit.
            fp.wait();

            const session = st.s.startSession();
            const sessionDB = session.getDatabase(dbName);
            const sessionColl1 = sessionDB.getCollection(collName1);
            const sessionColl2 = sessionDB.getCollection(collName2);

            // Make sure the session knows of a clusterTime inclusive of the resharding operation up
            // to the commit on the configsvr.
            assert.commandWorked(sessionColl2.insert({a: 2}));

            // Start txn.
            session.startTransaction({readConcern: {level: readConcernLevel}});
            assert.eq(2, sessionColl2.find().itcount());

            // Unset fp and wait for resharding to finish.
            fp.off();
            joinResharding();

            // Make sure the router is aware of the new (post-resharding) routing table for ns1
            assert.eq(2, coll1.find({y: 0}).itcount());

            // Now operate on coll1 within the transaction and expect to get a conflict.
            let err = assert.throwsWithCode(() => {
                if (command === 'find') {
                    sessionColl1.find().itcount();
                } else if (command === 'aggregate') {
                    sessionColl1.aggregate().itcount();
                } else if (command === 'update') {
                    assert.commandWorked(sessionColl1.updateMany({}, {$set: {c: 1}}));
                }
            }, [ErrorCodes.WriteConflict, ErrorCodes.SnapshotUnavailable]);
            assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

            // Transaction abort can race 'abort' before 'new transaction' command on participants,
            // so ensure there are no orphan transactions on participant.
            for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
                assert.commandWorked(db.runCommand({killSessions: [session.id]}));
            }
        }

        readConcerns.forEach((readConcern) => commands.forEach((command) => {
            runTest(readConcern, command);
        }));
    }
}

// Tests non-transaction reads with readConcern snapshot + atClusterTime.
{
    function runTest(command) {
        jsTest.log("Running non-txn snapshot read with command: " + command);

        const db = st.s.getDB('test_non_transaction_reads_with_' + command);
        const coll = db['sharded'];

        assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
        assert.commandWorked(st.s.adminCommand({
            moveChunk: coll.getFullName(),
            find: {x: -1},
            to: st.shard0.shardName,
            _waitForDelete: true
        }));
        assert.commandWorked(st.s.adminCommand({
            moveChunk: coll.getFullName(),
            find: {x: 1},
            to: st.shard1.shardName,
            _waitForDelete: true
        }));

        assert.commandWorked(coll.insertMany([{x: -1, a: 'sharded'}, {x: 1, a: 'sharded'}]));

        // Get a clusterTime inclusive of the above inserts, but not inclusive of the
        // drop-and-recreate operation that will follow.
        const clusterTimeBeforeDropAndRecreateAsUnsharded =
            db.getMongo().getClusterTime().clusterTime;

        // Drop and recreate
        assert(coll.drop());
        assert.commandWorked(coll.insert({x: 0, a: 'unsharded'}));

        // Make sure the router is aware of the new post-drop routing table for 'coll'.
        assert.eq(1, coll.find().itcount());

        // Read with snapshot readConcern and atClusterTime set at that clusterTime. Expect a
        // conflict to be thrown.
        let cmdResponse;
        if (command == 'find') {
            cmdResponse = coll.runCommand({
                find: coll.getName(),
                readConcern:
                    {level: 'snapshot', atClusterTime: clusterTimeBeforeDropAndRecreateAsUnsharded}
            });
        } else if (command == 'aggregate') {
            cmdResponse = coll.runCommand({
                aggregate: coll.getName(),
                pipeline: [],
                cursor: {},
                readConcern:
                    {level: 'snapshot', atClusterTime: clusterTimeBeforeDropAndRecreateAsUnsharded}
            });
        }

        assert.commandFailedWithCode(
            cmdResponse, [ErrorCodes.SnapshotUnavailable, ErrorCodes.StaleChunkHistory]);
    }

    ['find', 'aggregate'].forEach(command => {
        runTest(command);
    });
}

st.stop();
