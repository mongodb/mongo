/**
 * Tests that the placementConflictTime parameter of a transaction is honored.
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Test placementConflictTime", function () {
    let st;

    const dbName = jsTestName();

    const nameCollA = "collA";
    const nameCollB = "collB";

    const nsCollA = dbName + "." + nameCollA;
    const nsCollB = dbName + "." + nameCollB;

    const otherDb = "otherDb";
    const otherColl = "otherColl";

    before(() => {
        st = new ShardingTest({shards: 2});
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        st.s.getDB(otherDb).dropDatabase();
        st.s.getDB(dbName).dropDatabase();

        // Create the database db and define shard0 as the DBPrimary shard.
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

        // Create a collection "collA".
        assert.commandWorked(st.s.getCollection(nsCollA).insert({_id: 0, x: 1}));

        // Create a sharded collection, "collB", with the following chunks:
        // shard0: [x: -inf, x: 0)
        // shard1: [x: 0, x: +inf)
        assert.commandWorked(st.s.adminCommand({shardCollection: nsCollB, key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: nsCollB, middle: {x: 0}}));
        assert.commandWorked(st.s.adminCommand({moveChunk: nsCollB, find: {x: -10}, to: st.shard0.shardName}));
        assert.commandWorked(st.s.adminCommand({moveChunk: nsCollB, find: {x: 0}, to: st.shard1.shardName}));

        // Force refreshes to avoid getting stale config errors
        assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: nsCollA}));
        assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: nsCollA}));
        st.refreshCatalogCacheForNs(st.s, nsCollA);

        assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: nsCollB}));
        assert.commandWorked(st.shard1.adminCommand({_flushRoutingTableCacheUpdates: nsCollB}));
        st.refreshCatalogCacheForNs(st.s, nsCollB);
    });

    const runChunkMigrationTest = (readConcern, shouldThrow) => {
        const session = st.s.startSession();
        const sessionDB = session.getDatabase(dbName);

        // Start transaction
        session.startTransaction({readConcern: {level: readConcern}});

        // Run a find that will target shard0
        assert.eq(1, sessionDB.getCollection(nameCollA).find().itcount());

        // Move the sharded collection chunk to shard0 from shard1
        assert.commandWorked(st.s.adminCommand({moveChunk: nsCollB, find: {x: 10}, to: st.shard0.shardName}));

        if (shouldThrow) {
            let err = assert.throwsWithCode(() => {
                sessionDB.getCollection(nameCollB).find({x: 10}).itcount();
            }, [ErrorCodes.MigrationConflict]);
            assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

            session.abortTransaction();
        } else {
            sessionDB.getCollection(nameCollB).find({x: 10}).itcount();
            session.commitTransaction_forTesting();
        }
    };

    it("Verify txn with majority rc fails with MigrationConflict due to a chunk migration", () => {
        runChunkMigrationTest("majority", true);
    });

    it("Verify txn with local rc fails with MigrationConflict due to a chunk migration", () => {
        runChunkMigrationTest("local", true);
    });

    it("Verify txn doesn't fail due to chunk migrations with snapshot read concern", () => {
        runChunkMigrationTest("snapshot", false);
    });

    const runMovePrimaryTest = (readConcern, shouldFail) => {
        const session = st.s.startSession();

        const sessionDB = session.getDatabase(dbName);
        const otherSessionDB = session.getDatabase(otherDb);

        // Create the database 'otherDb' and define shard1 as its DBPrimary shard.
        assert.commandWorked(st.s.adminCommand({enableSharding: otherDb, primaryShard: st.shard1.shardName}));
        st.s.getDB(otherDb).getCollection(otherColl).insert({x: 1});

        // Start transaction
        session.startTransaction({readConcern: {level: readConcern}});

        // Run a find against collA within the txn
        assert.eq(1, sessionDB.getCollection(nameCollA).find().itcount());

        // Move the 'otherDb' to shard0 outside the txn to bump its DbVersion
        assert.commandWorked(st.s.adminCommand({movePrimary: otherDb, to: st.shard0.shardName}));
        st.refreshCatalogCacheForNs(st.s, otherDb + "." + otherColl);

        // Run a find against 'otherDb' within the txn
        if (shouldFail) {
            const err = assert.throwsWithCode(() => {
                otherSessionDB.getCollection(otherColl).find({x: 1}).itcount();
            }, [ErrorCodes.MigrationConflict]);

            assert.contains("TransientTransactionError", err.errorLabels, tojson(err));
            session.abortTransaction();
        } else {
            assert.eq(1, otherSessionDB.getCollection(otherColl).find({x: 1}).itcount());
            session.commitTransaction_forTesting();
        }
    };

    it("Verify txn with majority rc fails with MigrationConflict due to a db primary movement", () => {
        runMovePrimaryTest("majority", true);
    });

    it("Verify txn with local rc fails with MigrationConflict due to a db primary movement", () => {
        runMovePrimaryTest("local", true);
    });

    it("Verify txn with snapshot rc fails due to a db primary movement", () => {
        // Since we don't preserve the db placement history, the transaction should fail even with
        // 'snapshot' read concern.
        runMovePrimaryTest("snapshot", true);
    });

    it("Verify placementConflictTime is not checked if a database is created by the txn itself", () => {
        st.s.getDB(otherDb).dropDatabase();

        const session = st.s.startSession();
        const sessionDB = session.getDatabase(dbName);

        // Start the transaction
        session.startTransaction({readConcern: {level: "local"}});

        // Start the txn with a random operation to setup the placementConflictTime for this txn.
        assert.eq(1, sessionDB.getCollection(nameCollA).find().itcount());

        // Create a database within the txn.
        const otherSessionDB = session.getDatabase(otherDb);
        assert.commandWorked(otherSessionDB.createCollection(otherColl));

        // Verify that txn doesn't fail.
        assert.commandWorked(otherSessionDB.getCollection(otherColl).insert({x: 1}));
        assert.eq(1, otherSessionDB.getCollection(otherColl).find().itcount());

        session.commitTransaction_forTesting();
    });

    // Verifies that a transaction gets aborted if a participant steps down.
    // This is important because the placementConflictTime is an in memory variable on each participant shard.
    it.only("Verify transaction gets aborted if a participant steps down", () => {
        const session = st.s.startSession();
        const sessionDB = session.getDatabase(dbName);

        // Start transaction
        session.startTransaction();

        // Run a find that will target one of the shards
        assert.eq(1, sessionDB.getCollection(nameCollA).find({x: 1}).itcount());

        // Restart both shards
        st.shard1.rs.restart(st.shard1.rs.getPrimary());
        st.shard0.rs.restart(st.shard0.rs.getPrimary());

        st.shard1.rs.waitForPrimary();
        st.shard0.rs.waitForPrimary();

        // Confirm the transaction gets aborted
        assert.throwsWithCode(() => {
            sessionDB.getCollection(nameCollA).find({x: 1}).itcount();
        }, [ErrorCodes.NoSuchTransaction]);

        session.abortTransaction();
    });
});
