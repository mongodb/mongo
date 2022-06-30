/**
 * Tests that chunk migrations, collMod, createIndexes, and dropIndexes are prohibited on a
 * collection that is undergoing a resharding operation. Also tests that concurrent resharding
 * operations are prohibited.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const databaseName = "reshardingDb";
const collectionName = "coll";

const otherDatabaseName = 'reshardingDb2';
const otherCollectionName = 'coll2';
const otherNamespace = `${otherDatabaseName}.${otherCollectionName}`;

const donorOperationNamespace = 'config.localReshardingOperations.donor';

const indexCreatedByTest = {
    newKey: 1,
    oldKey: 1
};
const indexDroppedByTest = {
    x: 1
};

const prohibitedCommands = [
    {collMod: collectionName},
    {createIndexes: collectionName, indexes: [{name: "idx1", key: indexCreatedByTest}]},
    {dropIndexes: collectionName, index: indexDroppedByTest},
];

/**
 * @summary Goes through each of the commands specified in the prohibitedCommands array, executes
 * the command against the provided database and asserts that the command succeeded.
 * @param {*} database
 */
const assertCommandsSucceedAfterReshardingOpFinishes = (database) => {
    prohibitedCommands.forEach((command) => {
        jsTest.log(
            `Testing that ${tojson(command)} succeeds after the resharding operation finishes.`);
        assert.commandWorked(database.runCommand(command));
    });
};

/**
 * @summary Goes through each of the commands specified in the prohibitedCommands array,
 * executes the commands and asserts that the command failed with ReshardCollectionInProgress.
 * - In order for the dropIndexes command to fail correctly, the index its attempting to drop must
 * exist.
 * - In order for the createIndexes command to fail correctly, the index its attempting to create
 * must not exist already.
 * @param {*} database
 */
const assertCommandsFailDuringReshardingOp = (database) => {
    prohibitedCommands.forEach((command) => {
        jsTest.log(`Testing that ${tojson(command)} fails during resharding operation`);
        // The collMod is serialized with the resharding command, so we explicitly add an timeout to
        // the command so that it doesn't get blocked and timeout the test.
        if (command.hasOwnProperty('collMod') || command.hasOwnProperty('dropIndexes')) {
            command = Object.assign({}, command);
            command.maxTimeMS = 5000;
        }
        assert.commandFailedWithCode(
            database.runCommand(command),
            [ErrorCodes.ReshardCollectionInProgress, ErrorCodes.MaxTimeMSExpired]);
    });
};

const reshardingTest = new ReshardingTest({numDonors: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: `${databaseName}.${collectionName}`,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]}
    ],
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const sourceNamespace = sourceCollection.getFullName();

const waitUntilReshardingInitializedOnDonor = () => {
    const donor = new Mongo(topology.shards[donorShardNames[0]].primary);
    assert.soon(() => {
        let res = donor.getCollection(donorOperationNamespace).find().toArray();
        return res.length === 1;
    }, "timed out waiting for resharding initialization on donor shard");
};

/**
 * @summary The function that gets passed into reshardingTest.withReshardingInBackground
 * @callback DuringReshardingCallback
 * @param {String} tempNs - The temporary namespace used during resharding operations.
 */

/**
 * @summary A function that defines the surrounding environment for the tests surrounding the
 * prohibited commands during resharding. It sets up the resharding configuration and asserts that
 * the prohibited commands succeed once the resharding operation has completed.
 * @param {DuringReshardingCallback} duringReshardingFn
 * @param {Object} config
 * @param {number} config.expectedErrorCode
 * @param {Function} config.setup
 * @param {AfterReshardingCallback} afterReshardingFn
 */

const withReshardingInBackground =
    (duringReshardingFn,
     {setup = () => {}, expectedErrorCode, afterReshardingFn = () => {}} = {}) => {
        assert.commandWorked(sourceCollection.createIndex(indexDroppedByTest));
        setup();

        reshardingTest.withReshardingInBackground(
            {
                newShardKeyPattern: {newKey: 1},
                newChunks:
                    [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
            },
            duringReshardingFn,
            {expectedErrorCode: expectedErrorCode, afterReshardingFn: afterReshardingFn});
        assertCommandsSucceedAfterReshardingOpFinishes(mongos.getDB(databaseName));
        assert.commandWorked(sourceCollection.dropIndex(indexCreatedByTest));
    };

// Tests that the prohibited commands work if the resharding operation is aborted.
let awaitAbort;
withReshardingInBackground(() => {
    waitUntilReshardingInitializedOnDonor();
    assert.neq(null,
               mongos.getCollection("config.reshardingOperations").findOne({ns: sourceNamespace}));
    awaitAbort = startParallelShell(funWithArgs(function(sourceNamespace) {
                                        db.adminCommand({abortReshardCollection: sourceNamespace});
                                    }, sourceNamespace), mongos.port);
    // Wait for the coordinator to remove coordinator document from config.reshardingOperations
    // as a result of the recipients and donors transitioning to done due to abort.
    assert.soon(() => {
        const coordinatorDoc =
            mongos.getCollection("config.reshardingOperations").findOne({ns: sourceNamespace});

        return coordinatorDoc === null || coordinatorDoc.state === "aborting";
    });
}, {
    expectedErrorCode: ErrorCodes.ReshardCollectionAborted,
});
awaitAbort();

// Tests that the prohibited commands succeed if the resharding operation succeeds. During the
// operation it makes sure that the prohibited commands are rejected during the resharding
// operation.
withReshardingInBackground(() => {
    waitUntilReshardingInitializedOnDonor();

    jsTest.log(
        "About to test that the admin commands do not work while the resharding operation is in progress.");
    assert.commandFailedWithCode(
        mongos.adminCommand(
            {moveChunk: sourceNamespace, find: {oldKey: -10}, to: donorShardNames[1]}),
        ErrorCodes.LockBusy);
    assert.commandFailedWithCode(
        mongos.adminCommand({reshardCollection: otherNamespace, key: {newKey: 1}}),
        ErrorCodes.ReshardCollectionInProgress);

    assertCommandsFailDuringReshardingOp(sourceCollection.getDB());
}, {
    setup: () => {
        assert.commandWorked(mongos.adminCommand({enableSharding: otherDatabaseName}));
        assert.commandWorked(
            mongos.adminCommand({shardCollection: otherNamespace, key: {oldKey: 1}}));
    },
    afterReshardingFn: () => {
        jsTest.log("Join possible ongoing collMod command");
        assert.commandWorked(sourceCollection.runCommand("collMod"));
    },
});

reshardingTest.teardown();
})();
