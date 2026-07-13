/**
 * Tests that donor shard's secondaries correctly block reads and refresh metadata cache when
 * migration occurs.
 *
 * Legacy path: donor secondaries block reads during migration (phase 1) and during the
 * post-config-commit / pre-primary-refresh gap (phase 2).
 *
 * Authoritative path: phase 2 only — stale mongos + afterClusterTime secondary read during the
 * post-global-catalog-commit / pre-shard-catalog-install gap (hangBeforePostMigrationCommitRefresh
 * in recordCommitSuccess).
 *
 */

import {
    moveChunkParallel,
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * @summary This function executes a count command with read preference "secondary" and returns the
 * command response
 * @param {DBObject} db - The DB connection gotten from the shardingTest object that contains
 *     the collection the query will be run against.
 * @param {String} collectionName - The string name of the collection the read query will be run
 *     against
 * @param {Object} query the fields by which the read command should query by. For example {_id: 1,
 *     x: 4}, etc
 * @param {Object} optionalParams - non-required additional parameters to include into the read
 *     query.
 * * ^ Currently only supports readConcern and maxTimeMS
 * @returns {Object} - returns the an object with the total number of documents that matched the
 *     query on property 'n'
 */
const runReadCmdWithReadPrefSecondary = (db, collectionName, query, {readConcern, maxTimeMS}) => {
    return db.runCommand({
        count: collectionName,
        query,
        $readPreference: {mode: "secondary"},
        readConcern,
        maxTimeMS,
    });
};

/**
 * @summary This function does three things
 * 1.) Enables sharding for the database,
 * 2.) Shard the collection with the given namespace and uses _id as the shardkey,
 * 3.) Splits the chunk on the sharded collection based of {_id} as the middle of the chunk split.
 * @param {ShardingTest} shardingTest The instance of the initialized shardingTest
 * @param {String} dbName The string name of the database in which sharding will be enabled. Also
 *     the DB that contains the collection that will be sharded
 * @param {String} collectionNamespace The namespace (DB Name + Collection Name) of the collection
 *     that will be sharded. After sharding the namespace will be used for chunk splitting
 * @param {String} primaryShardName - Used to ensure the desired shard becomes the primary shard.
 */
const setupShardedCollection = (shardingTest, dbName, collectionNamespace, primaryShardName) => {
    assert.commandWorked(
        shardingTest.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}),
    );

    assert.commandWorked(
        shardingTest.s.adminCommand({
            shardCollection: collectionNamespace,
            key: {_id: 1},
        }),
    );
    assert.commandWorked(
        shardingTest.s.adminCommand({
            split: collectionNamespace,
            middle: {_id: 0},
        }),
    );
};

// TODO(SERVER-127253): Remove legacy branch once featureFlagAuthoritativeShardsDDL is always
// enabled.
function runLegacyCausalConsistencyTest(
    st,
    ns1,
    mongos0AlphaDB,
    mongos1AlphaDB,
    mongos1BetaDB,
    collName1,
    collName2,
) {
    // Starts a migration in a parallel shell to move the chunk [0, maxKey) in ns1 from shard0 to
    // shard1. Then it pauses the migration right before it enters the commit phase.
    jsTest.log("Starting legacy migration.");
    pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

    const staticMongod = MongoRunner.runMongod({});
    const joinMoveChunk = moveChunkParallel(
        staticMongod,
        st.s.host,
        {_id: 1},
        null,
        ns1,
        st.shard1.shardName,
        true /* expectSuccess */,
    );
    waitForMoveChunkStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

    // Sends a versioned read through mongos1, the second router, to cause a refresh on the secondary of
    // the donor shard. Since it runs while the migration is paused on 'chunkDataCommitted' the primary
    // is only blocking writes and if the secondary isn't checking for writes being blocked when sending
    // 'flushRoutingTableCacheUpdates' the secondary will serve reads for stale mongoses.

    jsTest.log("Sending a read query to donor shard's secondary.");

    assert.commandFailedWithCode(
        runReadCmdWithReadPrefSecondary(
            mongos1AlphaDB,
            collName1,
            {_id: 1},
            {maxTimeMS: 10000, readConcern: {level: "local"}},
        ),
        [ErrorCodes.MaxTimeMSExpired, ErrorCodes.StaleConfig],
    );

    // Allow the migration to commit and pause it before the donor shard's primary refreshes from the
    // config server.
    jsTest.log("Unpausing migration and enabling hangBeforePostMigrationCommitRefresh failpoint.");
    const hangBeforeRefreshFP = configureFailPoint(
        st.shard0,
        "hangBeforePostMigrationCommitRefresh",
    );
    unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);
    hangBeforeRefreshFP.wait();

    // Write to the migrated ns1 chunk. Run flushRouterConfig on the mongos to force it to refresh from
    // the config server when it does the write since otherwise it will route the request shard0 which
    // will be blocked behind the critical section. This insert will be used to test causal consistency
    // in the later read.
    jsTest.log("Sending insert through mongos0.");

    // Wait for effects of _configsvrCommitChunkMigration to be majority-committed snapshot on all
    // config server members since the mongos may refresh from a node other than the primary.
    st.configRS.awaitLastOpCommitted();

    assert.commandWorked(mongos0AlphaDB.adminCommand({flushRouterConfig: 1}));
    assert.commandWorked(
        mongos0AlphaDB.runCommand({
            insert: collName1,
            documents: [{_id: 2}],
            writeConcern: {w: "majority"},
        }),
    );

    // Bump the clusterTime of mongos1 to at least equal to the operationTime T for the
    // above write by writing to shard1. This is required for the afterClusterTime read
    // below to work since the 'afterCluterTime' of a command cannot be larger than the
    // current clusterTime of the mongod (i.e. shard0's secondary) that executes the
    // command. By bumping the clusterTime of mongos1, the clusterTime of shard0's
    // secondary will also get bumped to >= T due to clusterTime gossiping when we do
    // the afterClusterTime read.
    jsTest.log("Sending insert to other collection to bump cluster time.");
    assert.commandWorked(
        mongos1BetaDB.runCommand({
            insert: collName2,
            documents: [{_id: 1}],
            writeConcern: {w: "majority"},
        }),
    );
    assert.gt(
        bsonWoCompare(
            mongos1AlphaDB.getSession().getOperationTime(),
            mongos0AlphaDB.getSession().getOperationTime(),
        ),
        0,
    );

    // If the secondary doesn't wait behind the critical section like it should,
    // this read will find no matching documents because mongos1 will target
    // shard0 instead of shard1 (since mongos1's cache is stale).
    jsTest.log("Sending read query to secondary of shard0.");
    const totalDocsFound = runReadCmdWithReadPrefSecondary(
        mongos1AlphaDB,
        collName1,
        {_id: 2},
        {
            readConcern: {
                afterClusterTime: mongos0AlphaDB.getSession().getOperationTime(),
            },
        },
    ).n;
    assert.neq(totalDocsFound, 0);

    jsTest.log("Completing chunk migration.");
    hangBeforeRefreshFP.off();
    joinMoveChunk();

    MongoRunner.stopMongod(staticMongod);
}

function runAuthoritativeCausalConsistencyTest(
    st,
    ns1,
    mongos0AlphaDB,
    mongos0BetaDB,
    mongos1AlphaDB,
    mongos1BetaDB,
    collName1,
    collName2,
    preMigrationOperationTime,
) {
    jsTest.log("Starting authoritative migration.");
    const hangBeforeRefreshFP = configureFailPoint(
        st.shard0.rs.getPrimary(),
        "hangBeforePostMigrationCommitRefresh",
    );

    const staticMongod = MongoRunner.runMongod({});
    const joinMoveChunk = moveChunkParallel(
        staticMongod,
        st.s.host,
        {_id: 1},
        null,
        ns1,
        st.shard1.shardName,
        true /* expectSuccess */,
    );
    hangBeforeRefreshFP.wait();

    // The global catalog is committed but the recipient critical section is not released until
    // finalize, so a post-commit insert to the migrated chunk would block. {_id: 2} was inserted on
    // the donor before migration and cloned to the recipient during the migrate phase.
    st.configRS.awaitLastOpCommitted();

    // Establish a post-commit operationTime via mongos0 on an unrelated namespace. This mirrors the
    // legacy test's post-commit mongos0 write, but targets beta.bar (shard1) instead of alpha.foo
    // because the recipient critical section still blocks alpha.foo writes.
    jsTest.log("Sending post-commit insert through mongos0.");
    assert.commandWorked(mongos0AlphaDB.adminCommand({flushRouterConfig: 1}));
    assert.commandWorked(
        mongos0BetaDB.runCommand({
            insert: collName2,
            documents: [{_id: 2}],
            writeConcern: {w: "majority"},
        }),
    );
    const postCommitOperationTime = mongos0BetaDB.getSession().getOperationTime();
    assert.gt(
        bsonWoCompare(postCommitOperationTime, preMigrationOperationTime),
        0,
        "post-commit operationTime must be after the pre-migration insert",
    );

    // Bump the clusterTime of mongos1 to at least equal to the operationTime T for the above write.
    // This is required for the afterClusterTime read below since the afterClusterTime of a command
    // cannot be larger than the current clusterTime of the mongod (i.e. shard0's secondary) that
    // executes the command.
    jsTest.log("Sending insert to other collection to bump cluster time.");
    assert.commandWorked(
        mongos1BetaDB.runCommand({
            insert: collName2,
            documents: [{_id: 1}],
            writeConcern: {w: "majority"},
        }),
    );
    assert.gt(
        bsonWoCompare(mongos1AlphaDB.getSession().getOperationTime(), postCommitOperationTime),
        0,
    );

    // If the secondary doesn't wait behind the critical section like it should, this read will find
    // no matching documents because mongos1 will target shard0 instead of shard1 (since mongos1's
    // cache is stale).
    jsTest.log("Sending read query to secondary of shard0.");
    assert.commandFailedWithCode(
        runReadCmdWithReadPrefSecondary(
            mongos1AlphaDB,
            collName1,
            {_id: 2},
            {
                maxTimeMS: 10000,
                readConcern: {
                    afterClusterTime: postCommitOperationTime,
                },
            },
        ),
        [ErrorCodes.MaxTimeMSExpired, ErrorCodes.StaleConfig],
    );

    jsTest.log("Completing chunk migration.");
    hangBeforeRefreshFP.off();
    joinMoveChunk();

    MongoRunner.stopMongod(staticMongod);
}

const dbName1 = "alpha";
const dbName2 = "beta";

const collName1 = "foo";
const collName2 = "bar";

const ns1 = `${dbName1}.${collName1}`;
const ns2 = `${dbName2}.${collName2}`;

const st = new ShardingTest({
    mongos: 2,
    shards: 2,
    rs: {nodes: [{}, {rsConfig: {priority: 0}}]},
    causalConsistency: true,
});

const mongos0AlphaDB = st.s0.getDB(dbName1);
const mongos0BetaDB = st.s0.getDB(dbName2);
const mongos1AlphaDB = st.s1.getDB(dbName1);
const mongos1BetaDB = st.s1.getDB(dbName2);

const usesAuthoritativePath = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB("admin"),
    "AuthoritativeShardsDDL",
);

jsTest.log(`Sharding collection: ${ns1} and ${ns2}.`);
setupShardedCollection(st, dbName1, ns1, st.shard0.shardName);
setupShardedCollection(st, dbName2, ns2, st.shard1.shardName);

// Prime mongos1's routing table cache for ns1 so it remains stale after the migration commits.
assert.commandWorked(mongos1AlphaDB.runCommand({find: collName1, filter: {_id: 0}}));

if (usesAuthoritativePath) {
    assert.commandWorked(
        mongos0AlphaDB.runCommand({
            insert: collName1,
            documents: [{_id: 2}],
            writeConcern: {w: "majority"},
        }),
    );
    const preMigrationOperationTime = mongos0AlphaDB.getSession().getOperationTime();

    runAuthoritativeCausalConsistencyTest(
        st,
        ns1,
        mongos0AlphaDB,
        mongos0BetaDB,
        mongos1AlphaDB,
        mongos1BetaDB,
        collName1,
        collName2,
        preMigrationOperationTime,
    );
} else {
    runLegacyCausalConsistencyTest(
        st,
        ns1,
        mongos0AlphaDB,
        mongos1AlphaDB,
        mongos1BetaDB,
        collName1,
        collName2,
    );
}

st.stop();
