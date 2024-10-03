/**
 * Tests that change streams is able to find and return results when transition to or from dedicated
 * config server.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

// Enum for the types of CSRS transitions covered by this test.
const kTransitionType = Object.freeze({
    toDedicatedConfigServer: "transition to dedicated config server",
    fromDedicatedConfigServer: "transition from dedicated config server"
});

// Enum for the types of collection that may defined on test setup
const kCollSetupType =
    Object.freeze({unsharded: "unsharded collection", sharded: "sharded collection"});

// Initial cluster topology.
const st = new ShardingTest({
    shards: 2,
    configShard: true,
    rs: {nodes: 2, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}},
    other: {enableBalancer: true},
});

const configShardName = 'config';
const otherShardName =
    st.shard0.shardName === configShardName ? st.shard1.shardName : st.shard0.shardName;

function runtTransitionTestCases(transitionType, watchMode, collSetupType) {
    jsTest.log(`Testing ${transitionType} against changestreams of mode ${
        watchMode} on a cluster with a ${collSetupType}.`);

    // Setup the collection.
    const dbName = 'test';
    const collName = "coll";
    const nTotalDocsCreated = 10;
    const nDocsCreatedBeforeTransition = 4;

    const db = st.s.getDB(dbName);
    assert.commandWorked(db.dropDatabase());
    const primaryShardId =
        transitionType === kTransitionType.toDedicatedConfigServer ? 'config' : otherShardName;
    assert.commandWorked(
        db.adminCommand({enableSharding: db.getName(), primaryShard: primaryShardId}));

    const coll = assertDropAndRecreateCollection(db, collName);
    if (collSetupType === kCollSetupType.sharded) {
        st.shardColl(
            coll,
            {_id: 1},                          // key
            {_id: nTotalDocsCreated / 2},      // split
            {_id: nTotalDocsCreated / 2 + 1},  // move
            db.getName(),                      // dbName
            false                              // waitForDelete
        );
    }

    const cst = new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, db));

    // Test case 1: verify that a the CSRS transition does not impact an already opened change
    // stream.
    let csCursor = cst.getChangeStream({watchMode: watchMode, coll: coll});

    // Write some documents (and encode the related expected event); We ensure that in case of
    // sharded collection all shards get targeted by one or more ops.
    const expectedEvents = [];
    let bulk = coll.initializeUnorderedBulkOp();
    let addWriteOpAndExpectedEvent = (idValue) => {
        bulk.insert({_id: idValue});
        expectedEvents.push({
            operationType: 'insert',
            ns: {db: dbName, coll: collName},
            fullDocument: {_id: idValue},
            documentKey: {_id: idValue}
        });
    };
    for (let i = 0; i < nDocsCreatedBeforeTransition; i += 2) {
        addWriteOpAndExpectedEvent(i + 1);
        addWriteOpAndExpectedEvent(nTotalDocsCreated - i);
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    // Perform the transition.
    if (transitionType === kTransitionType.toDedicatedConfigServer) {
        moveDatabaseAndUnshardedColls(db, otherShardName);
        ShardTransitionUtil.transitionToDedicatedConfigServer(st);
    } else {
        assert.eq(kTransitionType.fromDedicatedConfigServer,
                  transitionType,
                  `Unsupported transition type: ${transitionType}`);
        assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
    }

    // Perform a second batch of writes after the transition.
    bulk = coll.initializeUnorderedBulkOp();
    for (let i = nDocsCreatedBeforeTransition; i < nTotalDocsCreated; i += 2) {
        addWriteOpAndExpectedEvent(i + 1);
        addWriteOpAndExpectedEvent(nTotalDocsCreated - i);
    }
    assert.commandWorked(bulk.execute({w: "majority"}));

    // Ensure that all the events occurred before and after the transition can be collected.
    let observedEvents =
        cst.assertNextChangesEqualUnordered({cursor: csCursor, expectedChanges: expectedEvents});

    // Test case 2: verify the expected output of a change stream resumed at a PIT that precedes
    // the transition (we use the first event by the previous test case as resume point).
    csCursor = null;
    const resumePoint = observedEvents[0]._id;

    try {
        csCursor =
            cst.getChangeStream({watchMode: watchMode, coll: coll, resumeAfter: resumePoint});
    } catch (err) {
        // A "resume change stream before a transition to dedicated config server" request is
        // expected to be rejected, due to the execution of a removeShard() behind the scenes that
        // makes part of the events potentially inaccessible.
        assert(transitionType === kTransitionType.toDedicatedConfigServer &&
                   ErrorCodes.ChangeStreamHistoryLost,
               `Unexpected error ${tojson(err)} while attempting to resume a change stream`);
        cst.cleanUp();
        return;
    }

    // On the other hand, a change stream resumed before a "transition from dedicated config server"
    // is expected to retrieve all the subsequent events.
    cst.assertNextChangesEqualUnordered(
        {cursor: csCursor, expectedChanges: expectedEvents.slice(1)});
    cst.cleanUp();
}

for (let watchMode of Object.values(ChangeStreamWatchMode)) {
    for (let collSetupType of Object.values(kCollSetupType)) {
        runtTransitionTestCases(kTransitionType.toDedicatedConfigServer, watchMode, collSetupType);
        runtTransitionTestCases(
            kTransitionType.fromDedicatedConfigServer, watchMode, collSetupType);
    }
}

st.stop();
