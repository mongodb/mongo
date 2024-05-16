/**
 * Tests that a change stream targeting an untracked collection is not disrupted by the concurrenct
 * execution of a movePrimary over its parent db.
 * @tags: [
 *   requires_fcv_80,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */
import {ChangeStreamTest, ChangeStreamWatchMode} from 'jstests/libs/change_stream_util.js';
import {assertCreateCollection} from 'jstests/libs/collection_drop_recreate.js';

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}},
});

const dbName = 'test';
const untrackedCollName = 'untrackedCollection';
const db = st.s.getDB(dbName);

assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
let untrackedColl = assertCreateCollection(db, untrackedCollName);

const cst = new ChangeStreamTest(
    ChangeStreamTest.getDBForChangeStream(ChangeStreamWatchMode.kCollection, db));

// 1. Verify that an open change stream can collect all the relevant events occurring before and
// after the execution of movePrimary().
let changeStreamCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {}}],
    collection: untrackedColl,
    aggregateOptions: {cursor: {batchSize: 0}},
});

untrackedColl.insertOne({_id: 'createdToEstablishResumeToken'});
untrackedColl.insertOne({_id: 'createdBeforeMovePrimary'});

assert.commandWorked(st.s.adminCommand({movePrimary: 'test', to: st.shard1.shardName}));

untrackedColl.insertOne({_id: 'createdAfterMovePrimary'});
untrackedColl.drop();

const expectedEvents = [
    {
        operationType: 'insert',
        ns: {db: dbName, coll: untrackedColl.getName()},
        fullDocument: {_id: 'createdToEstablishResumeToken'},
        documentKey: {_id: 'createdToEstablishResumeToken'}
    },
    {
        operationType: 'insert',
        ns: {db: dbName, coll: untrackedColl.getName()},
        fullDocument: {_id: 'createdBeforeMovePrimary'},
        documentKey: {_id: 'createdBeforeMovePrimary'}
    },
    {
        operationType: 'insert',
        ns: {db: dbName, coll: untrackedColl.getName()},
        fullDocument: {_id: 'createdAfterMovePrimary'},
        documentKey: {_id: 'createdAfterMovePrimary'}
    },
    {
        operationType: 'drop',
        ns: {db: dbName, coll: untrackedColl.getName()},
    },
    {
        operationType: 'invalidate',
    }
];

const observedEvents = cst.assertNextChangesEqual(
    {cursor: changeStreamCursor, expectedChanges: expectedEvents, expectInvalidate: true});

// 2. Verify that a change stream started pulling past events can also collect all the relevant
// events occurring before and after the execution of removeShard(). The first event retrieved from
// the previous test case is used as resume token.

changeStreamCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {showSystemEvents: true, resumeAfter: observedEvents[0]._id}}],
    collection: untrackedColl,
    aggregateOptions: {cursor: {batchSize: 0}},
});

cst.assertNextChangesEqual(
    {cursor: changeStreamCursor, expectedChanges: expectedEvents.slice(1), expectInvalidate: true});

st.stop();
