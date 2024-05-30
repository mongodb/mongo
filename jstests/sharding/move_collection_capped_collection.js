/**
 * Tests for capped collection functionality of the move collection feature.
 *
 * @tags: [
 *  requires_fcv_80,
 *  featureFlagMoveCollection,
 *  assumes_balancer_off,
 *  requires_capped
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createUnshardedCollection(
    {ns: ns, primaryShardName: donorShardNames[0], collOptions: {capped: true, size: 4096}});

// Insert more than one document to it. This tests that capped collections can clone multiple docs.
const numDocs = 30;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(sourceCollection.insert({x: i}));
}

const preReshardingOrder = sourceCollection.find({}).toArray();
assert.commandWorked(sourceCollection.insert({x: 31}));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withMoveCollectionInBackground(
    {
        toShard: recipientShardNames[0],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        // Test delete oplog application rules.
        assert.commandWorked(sourceCollection.remove({x: 31}, {justOne: true}));
    });

const st = reshardingTest._st;
let collEntry = st.s.getDB('config').getCollection('collections').findOne({_id: ns});
assert.eq(collEntry._id, ns);
assert.eq(collEntry.unsplittable, true);
assert.eq(collEntry.key, {_id: 1});
assert.eq(st.s.getDB(dbName).getCollection(collName).isCapped(), true);
assert.eq(st.s.getDB(dbName).getCollection(collName).countDocuments({}), numDocs);
assert.eq(st.rs0.getPrimary().getDB(dbName).getCollection(collName).countDocuments({}), 0);
assert.eq(st.rs1.getPrimary().getDB(dbName).getCollection(collName).countDocuments({}), numDocs);

// Order matches after resharding.
const postReshardingOrder = st.s.getDB(dbName).getCollection(collName).find({}).toArray();
assert.eq(preReshardingOrder.length, postReshardingOrder.length);
for (let i = 0; i < preReshardingOrder.length; i++) {
    assert.eq(preReshardingOrder[i], postReshardingOrder[i]);
}

reshardingTest.teardown();
