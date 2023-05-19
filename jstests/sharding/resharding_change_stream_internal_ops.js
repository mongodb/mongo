// Tests that internal resharding ops are exposed as change stream events during a
// resharding operation. Internal resharding ops include:
// 1. reshardBegin
// 2. reshardDoneCatchUp
//
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
//   uses_atclustertime,
//
// ]
(function() {
"use strict";

load('jstests/libs/change_stream_util.js');
load("jstests/libs/discover_topology.js");
load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

// Use a higher frequency for periodic noops to speed up the test.
const reshardingTest = new ReshardingTest({
    numDonors: 2,
    numRecipients: 1,
    reshardInPlace: false,
    periodicNoopIntervalSecs: 1,
    writePeriodicNoops: true
});
reshardingTest.setup();

const kDbName = "reshardingDb";
const collName = "coll";
const ns = kDbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]}
    ],
    primaryShardName: donorShardNames[0]
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const cstDonor0 = new ChangeStreamTest(donor0.getDB(kDbName));
let changeStreamsCursorDonor0 = cstDonor0.startWatchingChanges(
    {pipeline: [{$changeStream: {showMigrationEvents: true}}], collection: collName});

const donor1 = new Mongo(topology.shards[donorShardNames[1]].primary);
const cstDonor1 = new ChangeStreamTest(donor1.getDB(kDbName));
let changeStreamsCursorDonor1 = cstDonor1.startWatchingChanges(
    {pipeline: [{$changeStream: {showMigrationEvents: true}}], collection: collName});

const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);
const cstRecipient0 = new ChangeStreamTest(recipient0.getDB(kDbName));

let reshardingUUID;
let changeStreamsCursorRecipient0;

reshardingTest.withReshardingInBackground(
    {
        // If a donor is also a recipient, the donor state machine will run renameCollection with
        // {dropTarget : true} rather than running drop and letting the recipient state machine run
        // rename at the end of the resharding operation. So, we ensure that only one of the donor
        // shards will also be a recipient shard in order to verify that neither the rename with
        // {dropTarget : true} nor the drop command are picked up by the change streams cursor.
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
        ],
    },
    (tempNs) => {
        // Wait until participants are aware of the resharding operation.
        reshardingTest.awaitCloneTimestampChosen();

        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne();
        reshardingUUID = coordinatorDoc._id;

        changeStreamsCursorRecipient0 = cstRecipient0.startWatchingChanges({
            pipeline: [{$changeStream: {showMigrationEvents: true, allowToRunOnSystemNS: true}}],
            collection: tempNs.substring(tempNs.indexOf('.') + 1)
        });

        // Check for reshardBegin event on both donors.
        const expectedReshardBeginEvent = {
            reshardingUUID: reshardingUUID,
            operationType: "reshardBegin",
            ns: {db: kDbName, coll: collName},
        };

        const reshardBeginDonor0Event =
            cstDonor0.getNextChanges(changeStreamsCursorDonor0, 1, false /* skipFirstBatch */);

        assertChangeStreamEventEq(reshardBeginDonor0Event[0], expectedReshardBeginEvent);

        const reshardBeginDonor1Event =
            cstDonor1.getNextChanges(changeStreamsCursorDonor1, 1, false /* skipFirstBatch */);
        assertChangeStreamEventEq(reshardBeginDonor1Event[0], expectedReshardBeginEvent);
    },
    {
        postDecisionPersistedFn: () => {
            // Check for reshardDoneCatchUp event on the recipient.
            const expectedReshardDoneCatchUpEvent = {
                reshardingUUID: reshardingUUID,
                operationType: "reshardDoneCatchUp",
            };

            const reshardDoneCatchUpEvent = cstRecipient0.getNextChanges(
                changeStreamsCursorRecipient0, 1, false /* skipFirstBatch */)[0];

            // Ensure that the 'reshardingDoneCatchUp' event has an 'ns' field of the format
            // '{ns: kDbName, coll: "system.resharding.<>"}.
            assert(reshardDoneCatchUpEvent.ns, reshardDoneCatchUpEvent);
            assert.eq(reshardDoneCatchUpEvent.ns.db, kDbName, reshardDoneCatchUpEvent);
            assert(reshardDoneCatchUpEvent.ns.coll.startsWith("system.resharding."),
                   reshardDoneCatchUpEvent);
            delete reshardDoneCatchUpEvent.ns;

            assertChangeStreamEventEq(reshardDoneCatchUpEvent, expectedReshardDoneCatchUpEvent);
        }
    });

reshardingTest.teardown();
})();
