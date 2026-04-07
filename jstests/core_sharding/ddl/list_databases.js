/**
 * Tests listDatabases after movePrimary, moveCollection and moveChunk in cluster environment.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_2_or_more_shards,
 *   assumes_unsharded_collection,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getRandomShardName} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";

// Retries getRandomShardName + enableSharding to handle transient errors such as
// FailedToSatisfyReadPreference (config server stepdown) and ShardNotFound (draining shard).
function enableShardingWithRetry(conn, dbName, excludeShards = []) {
    let primaryShard;
    assert.soon(function () {
        try {
            primaryShard = getRandomShardName(conn, excludeShards);
            assert.commandWorked(conn.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));
            return true;
        } catch (e) {
            if (e.code === ErrorCodes.ShardNotFound || e.code === ErrorCodes.FailedToSatisfyReadPreference) {
                return false;
            }
            throw e;
        }
    });
    return primaryShard;
}

describe("listDatabases after shard topology changes", function () {
    let testPrimaryDB;
    let testCollectionDB;
    let testChunkDB;

    before(function () {
        testPrimaryDB = db.getSiblingDB(jsTestName() + "_test_primary");
        testCollectionDB = db.getSiblingDB(jsTestName() + "_test_collection");
        testChunkDB = db.getSiblingDB(jsTestName() + "_test_chunk");

        testPrimaryDB.dropDatabase();
        testCollectionDB.dropDatabase();
        testChunkDB.dropDatabase();
    });

    after(function () {
        testPrimaryDB.dropDatabase();
        testCollectionDB.dropDatabase();
        testChunkDB.dropDatabase();
    });
    // The movePrimary case is not reliable under continuous stepdowns.
    const itWithMovePrimary =
        TestData?.runningWithShardStepdowns || TestData?.runningWithConfigStepdowns ? it.skip : it;
    itWithMovePrimary("should list the database after movePrimary", function () {
        const originalPrimary = enableShardingWithRetry(db, testPrimaryDB.getName());

        assert.commandWorked(testPrimaryDB.getCollection("coll1").insertOne({x: 1}));

        let res = assert.commandWorked(
            testPrimaryDB.adminCommand({listDatabases: 1, filter: {name: testPrimaryDB.getName()}}),
        );
        assert.eq(1, res.databases.length);
        assert.eq(testPrimaryDB.getName(), res.databases[0].name);

        const newPrimary = getRandomShardName(db, [originalPrimary]);

        let expectedErrorCodes = [
            // Caused by a concurrent movePrimary operation on the same database but a
            // different destination shard.
            ErrorCodes.ConflictingOperationInProgress,
            // The target shard may start draining between getRandomShardName and movePrimary
            // in suites that dynamically add/remove shards.
            ErrorCodes.ShardNotFound,
            // Due to a stepdown of the donor during the cloning phase, the movePrimary
            // operation failed. It is not automatically recovered, but any orphaned data on
            // the recipient has been deleted.
            7120202,
            // In the FSM tests, there is a chance that there are still some User
            // collections left to clone. This occurs when a MovePrimary joins an already
            // existing MovePrimary command that has purposefully triggered a failpoint.
            9046501,
        ];

        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({movePrimary: testPrimaryDB.getName(), to: newPrimary}),
            expectedErrorCodes,
        );

        res = assert.commandWorked(
            testPrimaryDB.adminCommand({listDatabases: 1, filter: {name: testPrimaryDB.getName()}}),
        );
        assert.eq(1, res.databases.length);
        assert.eq(testPrimaryDB.getName(), res.databases[0].name);
    });

    it("should list the database with unsharded collection after moveCollection", function () {
        const originalPrimary = enableShardingWithRetry(db, testCollectionDB.getName());

        const coll1 = testCollectionDB.coll1;
        assert.commandWorked(coll1.insertOne({x: 1}));

        let res = assert.commandWorked(
            testCollectionDB.adminCommand({listDatabases: 1, filter: {name: testCollectionDB.getName()}}),
        );
        assert.eq(1, res.databases.length);
        assert.eq(testCollectionDB.getName(), res.databases[0].name);

        const newPrimary = getRandomShardName(db, [originalPrimary]);

        let expectedErrorCodes = [
            ErrorCodes.ReshardCollectionInterruptedDueToFCVChange,
            ErrorCodes.CommandNotSupported,
            ErrorCodes.Interrupted,
            ErrorCodes.ReshardCollectionAborted,
            ErrorCodes.ShardNotFound,
        ];

        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({moveCollection: coll1.getFullName(), toShard: newPrimary}),
            expectedErrorCodes,
        );

        res = assert.commandWorked(
            testCollectionDB.adminCommand({listDatabases: 1, filter: {name: testCollectionDB.getName()}}),
        );
        assert.eq(1, res.databases.length);
        assert.eq(testCollectionDB.getName(), res.databases[0].name);
    });

    it("should list the database with sharded collection after moveChunk", function () {
        const originalPrimary = enableShardingWithRetry(db, testChunkDB.getName());

        const coll1 = testChunkDB.coll1;
        assert.commandWorked(coll1.createIndex({x: 1}));
        assert.commandWorked(db.adminCommand({shardCollection: coll1.getFullName(), key: {x: 1}}));
        assert.commandWorked(coll1.insertOne({x: 1}));

        let res = assert.commandWorked(
            testChunkDB.adminCommand({listDatabases: 1, filter: {name: testChunkDB.getName()}}),
        );
        assert.eq(1, res.databases.length);
        assert.eq(testChunkDB.getName(), res.databases[0].name);

        const newShard = getRandomShardName(db, [originalPrimary]);

        let expectedErrorCodes = [
            ErrorCodes.ConflictingOperationInProgress,
            ErrorCodes.ShardNotFound,
            ErrorCodes.CommandFailed,
            ErrorCodes.OperationFailed,
        ];

        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({moveChunk: coll1.getFullName(), find: {x: 1}, to: newShard}),
            expectedErrorCodes,
        );

        res = assert.commandWorked(testChunkDB.adminCommand({listDatabases: 1, filter: {name: testChunkDB.getName()}}));
        assert.eq(1, res.databases.length);
        assert.eq(testChunkDB.getName(), res.databases[0].name);
    });
});
