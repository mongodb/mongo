/**
 * Test to make sure that a write by one tenant can't advance the resume token of another tenant. If
 * it can happen then during a split a migrating tenant can wind up with a resume token greater than
 * the split operation's blockTS and we could skip events when resuming on the recipient.
 * @tags: [
 *     serverless,
 *     requires_fcv_71
 * ]
 */
// TODO SERVER-76309: re-purpose this test to show that the resume token does advance with the
// global oplog, or remove the test in favour of existing coverage elsewhere.
load("jstests/serverless/libs/change_collection_util.js");

(function() {
const tenantIds = [ObjectId(), ObjectId()];
const rst = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 3,
    nodeOptions: {setParameter: {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1}}
});

const primary = rst.getPrimary();
const tenant1Conn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, tenantIds[0]);
const tenant2Conn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, tenantIds[1]);
const tenant1DB = tenant1Conn.getDB("test");
const tenant2DB = tenant2Conn.getDB("test");
rst.setChangeStreamState(tenant1Conn, true);
rst.setChangeStreamState(tenant2Conn, true);

// Open a stream on the test collection, and write a document to it.
const csCursor = tenant1DB.coll.watch();
assert.commandWorked(tenant1DB.coll.insert({}));
assert.soon(() => csCursor.hasNext());
const monitoredEvent = csCursor.next();

// Write an event to an un-monitored collection for the same tenant. Since this event is written
// into that tenant's change collection, it will cause the PBRT to advance even though that event is
// not relevant to the stream we have opened. When we see a PBRT that is greater than the timestamp
// of the last event (stored in 'monitoredEvent'), we know it must be a synthetic high-water-mark
// token.
//
// Note that the first insert into the un-monitored collection may not be enough to advance the
// PBRT; some passthroughs will group the un-monitored write into a transaction with the monitored
// write, giving them the same timestamp. We put the un-monitored insert into the assert.soon loop,
// so that it will eventually get its own transaction with a new timestamp.
let hwmToken = null;
assert.soon(() => {
    assert.commandWorked(tenant1DB.coll2.insert({}));
    assert.eq(csCursor.hasNext(), false);
    hwmToken = csCursor.getResumeToken();
    assert.neq(undefined, hwmToken);
    return bsonWoCompare(hwmToken, monitoredEvent._id) > 0;
});

// Open a change stream on tenant 2 so we can observe a write that happens and verify that write
// advanced the global oplog timestamp.
const csCursor2 = tenant2DB.coll.watch();
let tenant2Event = null;
assert.soon(() => {
    assert.commandWorked(tenant2DB.coll.insert({}));
    assert.soon(() => csCursor2.hasNext());
    tenant2Event = csCursor2.next();
    return bsonWoCompare(tenant2Event._id, hwmToken) > 0;
});

// Try to get a new resume token for tenant 1. We shouldn't be able to get a new resume token
// greater than the last resume token we got.
assert.eq(csCursor.hasNext(), false);
hwmToken2 = csCursor.getResumeToken();
assert.neq(undefined, hwmToken2);
assert.eq(bsonWoCompare(hwmToken, hwmToken2), 0);

rst.stopSet();
})();
