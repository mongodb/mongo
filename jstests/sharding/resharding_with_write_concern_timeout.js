/**
 * Tests that resharding is resilient to sporadic write concern timeouts.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_83
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from 'jstests/libs/parallelTester.js';
import {ShardingTest} from 'jstests/libs/shardingtest.js';

function pauseReshardingBeforeBlockingWrites(configRS) {
    const node = configRS.getPrimary();
    return configureFailPoint(node, "reshardingPauseCoordinatorBeforeBlockingWrites");
}

function runMoveCollection(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveCollection: ns, toShard});
}

function stepUpNewPrimary(rst) {
    const oldPrimary = rst.getPrimary();
    const oldSecondary = rst.getSecondary();
    assert.neq(oldPrimary, oldSecondary);
    rst.stepUp(rst.getSecondary(), {awaitReplicationBeforeStepUp: false});
    const newPrimary = rst.getPrimary();
    assert.eq(newPrimary, oldSecondary);
}

function setFailWriteConcernFailpointOnAllNodes(listOfReplSets) {
    // Set activation probability to less than 1 so that as long as there are retries,
    // moveCollection will eventually succeed.
    let activationProbability = 0.5;
    let failpoints = [];
    for (let replSetTest of listOfReplSets) {
        for (let node of replSetTest.nodes) {
            failpoints.push(configureFailPoint(node,
                                               "failWaitForWriteConcernIfTimeoutSet",
                                               {errorCode: ErrorCodes.WriteConcernTimeout},
                                               {activationProbability}));
        }
    }
    return failpoints;
}

function testWriteConcernBasic(st) {
    // Set up the collection to reshard.
    const dbName = "testDbBasic";
    const collName = "testColl";
    const ns = dbName + '.' + collName;
    const testColl = st.s.getCollection(ns);

    // This test verifies moveCollection is resilient against WriteConcernTimeout errors. It works
    // by setting a failpoint to make the write concern wait on the donor, recipient and coordinator
    // fail with some probability, and then asserting that the moveCollecton operation still
    // succeeds.

    // The router initiates a resharding operation by running running the
    // `_shardsvrReshardCollection` against the primary shard. The command is run writeConcern
    // "majority" so it involves waiting for write concern separately from the resharding machinery
    // so it doesn't have WriteConcernError retries. There isn't a good way between a
    // WriteConcernError thrown before versus after resharding has been initailized. To simplify the
    // test, we completely avoid WriteConcernError errors before resharding is initialized by not
    // setting the write concern failpoint on the primary shard. For this reason, the primary shard
    // must not be the donor or the only recipient for the moveCollection operation. It also must
    // not be the coordinator for the operation (i.e. cannot be shard0 since in the config shard
    // suite, shard0 is the embedded config server). Given this, the test setup is as follows:
    // 1. Set the primary to shard 1.
    // 2. Move the collection from shard 1 to shard 0.
    // 3. Set the failWriteConcernFailpoint on shard 0, shard 2, and the config shard.
    // 4. Move the collection from shard 0 to shard 2.

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
    assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    assert.commandWorked(testColl.createIndex({x: 1}));

    const moveThreadInitial = new Thread(runMoveCollection, st.s.host, ns, st.shard0.shardName);
    moveThreadInitial.start();

    jsTest.log.info("Start waiting for initial moveCollection to finish");
    assert.commandWorked(moveThreadInitial.returnData());
    jsTest.log.info("Finished waiting for initial moveCollection to finish");

    const blockingWriteFailPoint = pauseReshardingBeforeBlockingWrites(st.configRS);
    let failpoints = setFailWriteConcernFailpointOnAllNodes([st.rs0, st.rs2, st.configRS]);

    const moveThreadForTest = new Thread(runMoveCollection, st.s.host, ns, st.shard2.shardName);
    moveThreadForTest.start();

    // TODO Remove the find command when SERVER-109322 is done.
    // The find command triggers a catalog cache refresh which could throw a WriteConcernTimeout.
    // Using assert.soon so that find eventually works.
    assert.soon(() => {
        let res = testColl.find().explain();
        return res.ok === 1;
    });

    blockingWriteFailPoint.wait();
    assert.commandWorked(testColl.insert([{x: -3}, {x: 3}]));
    blockingWriteFailPoint.off();

    jsTest.log.info("Start waiting for test moveCollection to finish");
    assert.commandWorked(moveThreadForTest.returnData());
    jsTest.log.info("Finished waiting for test moveCollection to finish");

    failpoints.forEach(fp => fp.off());
}

function testWriteConcernFailover(st) {
    const dbName = "testDbFailover";
    const collName = "testColl";
    const ns = dbName + '.' + collName;
    const testColl = st.s.getCollection(ns);

    // This test verifies moveCollection is resilient against WriteConcernTimeout errors. It works
    // by setting a failpoint to make the write concern wait on the donor, recipient and coordinator
    // fail with some probability, and then asserting that the moveCollecton operation still
    // succeeds.

    // The router initiates a resharding operation by running running the
    // `_shardsvrReshardCollection` against the primary shard. The command is run writeConcern
    // "majority" so it involves waiting for write concern separately from the resharding machinery
    // so it doesn't have WriteConcernError retries. There isn't a good way between a
    // WriteConcernError thrown before versus after resharding has been initailized. To simplify the
    // test, we completely avoid WriteConcernError errors before resharding is initialized by not
    // setting the write concern failpoint on the primary shard. For this reason, the primary shard
    // must not be the donor or the only recipient for the moveCollection operation. It also must
    // not be the coordinator for the operation (i.e. cannot be shard0 since in the config shard
    // suite, shard0 is the embedded config server). Given this, the test setup is as follows:
    // 1. Set the primary to shard 1.
    // 2. Move the collection from shard 1 to shard 0.
    // 3. Set the failWriteConcernFailpoint on shard 0, shard 2, and the config shard.
    // 4. Move the collection from shard 0 to shard 2.

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
    assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    assert.commandWorked(testColl.createIndex({x: 1}));

    const moveThreadInitial = new Thread(runMoveCollection, st.s.host, ns, st.shard0.shardName);
    moveThreadInitial.start();

    jsTest.log.info("Start waiting for initial moveCollection to finish");
    assert.commandWorked(moveThreadInitial.returnData());
    jsTest.log.info("Finished waiting for initial moveCollection to finish");

    let failpoints = setFailWriteConcernFailpointOnAllNodes([st.rs0, st.rs2, st.configRS]);

    const moveThreadForTest = new Thread(runMoveCollection, st.s.host, ns, st.shard2.shardName);
    moveThreadForTest.start();

    jsTest.log.info("Triggering a failover on shard0");
    stepUpNewPrimary(st.rs0);
    jsTest.log.info("Triggering a failover on shard2");
    stepUpNewPrimary(st.rs2);

    jsTest.log.info("Start waiting for test moveCollection to finish");
    assert.commandWorked(moveThreadForTest.returnData());
    jsTest.log.info("Finished waiting for test moveCollection to finish");

    failpoints.forEach(fp => fp.off());
}

function runTests() {
    // TODO Do not explicitly set this feature flag after SERVER-109032 is done.
    const featureFlagReshardingVerification = false;
    // TODO Do not explicitly set this feature flag after SERVER-108476 is done.
    const featureFlagReshardingCloneNoRefresh = false;
    const st = new ShardingTest({
        shards: 3,
        rs: {
            nodes: 3,
            setParameter: {featureFlagReshardingVerification, featureFlagReshardingCloneNoRefresh}
        },
        other: {
            configOptions: {
                setParameter:
                    {featureFlagReshardingVerification, featureFlagReshardingCloneNoRefresh}
            }
        }
    });
    testWriteConcernBasic(st);
    testWriteConcernFailover(st);

    st.stop();
}

runTests();
