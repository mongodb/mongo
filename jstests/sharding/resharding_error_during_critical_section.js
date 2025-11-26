/**
 * Tests that the resharding coordinator correctly handles critical timeout and abort while waiting
 * for responses for commands against donors or recipients in the critical section.
 *
 * @tags: [
 *   requires_fcv_83,
 *   featureFlagReshardingVerification,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runMoveCollection(mongosHost, ns, toShard) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({
        moveCollection: ns,
        toShard,
    });
}

const st = new ShardingTest({
    shards: 2,
    other: {
        configOptions: {
            setParameter: {
                // Set a large threshold to make each resharding operation below able to enter the
                // critical section quickly even when running on slow build variants.
                remainingReshardingOperationTimeThresholdMillis: 5000,
            },
        },
    },
});
const shard0Primary = st.rs0.getPrimary();
const configPrimary = st.configRS.getPrimary();

const dbName = "testDb";
const testDb = st.s.getDB(dbName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const testCriticalSectionTimeoutMS = 1000;
const cmdBlockTimeoutMS = 60 * 60 * 1000;
let testNum = 0;

function configureFailPointToBlockCommand(node, cmdName, numSkips) {
    return configureFailPoint(
        node,
        "failCommand",
        {
            failCommands: [cmdName],
            blockConnection: true,
            blockTimeMS: cmdBlockTimeoutMS,
            failInternalCommands: true,
        },
        {skip: numSkips},
    );
}

function testCriticalSectionTimeoutWhileWaiting(cmdName, numSkips) {
    jsTest.log("Test resharding critical section timeout while coordinator is waiting for responses for " + cmdName);

    const collName = "testColl" + testNum++;
    const ns = dbName + "." + collName;
    const testColl = testDb.getCollection(collName);
    assert.commandWorked(testColl.createIndex({x: 1}));
    assert.commandWorked(
        testColl.insert([
            {_id: -1, x: -1, y: -1},
            {_id: 1, x: 1, y: 1},
        ]),
    );

    const originalCriticalSectionTimeout = assert.commandWorked(
        configPrimary.adminCommand({
            setParameter: 1,
            reshardingCriticalSectionTimeoutMillis: testCriticalSectionTimeoutMS,
        }),
    ).was;
    const fp = configureFailPointToBlockCommand(shard0Primary, cmdName, numSkips);

    const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
    moveThread.start();
    moveThread.join();
    assert.commandFailedWithCode(moveThread.returnData(), ErrorCodes.ReshardingCriticalSectionTimeout);

    fp.off();
    assert.commandWorked(
        configPrimary.adminCommand({
            setParameter: 1,
            reshardingCriticalSectionTimeoutMillis: originalCriticalSectionTimeout,
        }),
    );
}

function testAbortWhileWaiting(cmdName, numSkips) {
    jsTest.log("Test aborting resharding while coordinator is waiting for responses for " + cmdName);

    const collName = "testColl" + testNum++;
    const ns = dbName + "." + collName;
    const testColl = testDb.getCollection(collName);
    assert.commandWorked(testColl.createIndex({x: 1}));
    assert.commandWorked(
        testColl.insert([
            {_id: -1, x: -1, y: -1},
            {_id: 1, x: 1, y: 1},
        ]),
    );

    const fp = configureFailPointToBlockCommand(shard0Primary, cmdName, numSkips);

    const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
    moveThread.start();

    fp.wait();
    assert.commandWorked(st.s.adminCommand({abortMoveCollection: ns}));

    moveThread.join();
    assert.commandFailedWithCode(moveThread.returnData(), ErrorCodes.ReshardCollectionAborted);
    fp.off();
}

const cmdsToBlock = [
    {
        cmdName: "_flushReshardingStateChange",
        // Skip the refreshes for transitioning to "cloning" and to "applying" to test the error
        // during critical section. This count assumes that _shardsvrReshardRecipientClone is
        // disabled.
        numSkips: 2,
    },
    {
        cmdName: "_shardsvrReshardingDonorFetchFinalCollectionStats",
        // No skipping.
        numSkips: 0,
    },
];

for (const {cmdName, numSkips} of cmdsToBlock) {
    testCriticalSectionTimeoutWhileWaiting(cmdName, numSkips);
    testAbortWhileWaiting(cmdName, numSkips);
}

st.stop();
