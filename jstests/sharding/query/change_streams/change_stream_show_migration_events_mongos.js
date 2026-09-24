// Tests the undocumented 'showMigrationEvents' option for change streams opened on mongos.
//
// @tags: [
//   requires_majority_read_concern,
//   requires_fcv_91,
//   requires_sharding,
//   uses_change_streams,
// ]
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 1},
    other: {
        rsOptions: {
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
        },
    },
});

const mongos = st.s;
const mongosDB = mongos.getDB("test");
const mongosColl = mongosDB.getCollection("mongos_migration_events");

// Enable sharding and place one chunk on each shard so the mongos change stream opens
// cursors on both shards before any migration occurs.
assert.commandWorked(
    mongos.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}),
);
assert.commandWorked(
    mongos.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}),
);
assert.commandWorked(mongos.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({split: mongosColl.getFullName(), middle: {_id: 10}}));
assert.commandWorked(
    mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: 20},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

// Open a change stream on mongos with showMigrationEvents enabled.
const changeStreamTestMongos = new ChangeStreamTest(mongosDB);
const changeStreamMongos = changeStreamTestMongos.startWatchingChanges({
    pipeline: [{$changeStream: {showMigrationEvents: true}}],
    collection: mongosColl,
});

function makeMongosEvent(docId, opType, fromMigrate = undefined) {
    let doc = {
        operationType: opType,
        documentKey: {_id: docId},
        ns: {db: "test", coll: "mongos_migration_events"},
    };
    if (opType === "insert") {
        doc.fullDocument = {_id: docId};
    }
    if (fromMigrate) {
        doc.fromMigrate = true;
    }
    return doc;
}

// Insert documents into both shards.
assert.commandWorked(mongosColl.insert({_id: -1}));
assert.commandWorked(mongosColl.insert({_id: 5}));
assert.commandWorked(mongosColl.insert({_id: 15}));

// Migrate a chunk while the donor shard still owns another chunk. This ensures the mongos
// change stream already has an open cursor on both the donor and recipient shards.
assert.commandWorked(
    mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: 5},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

// The mongos stream should observe the normal inserts as well as the migration-related
// delete on the donor and insert on the recipient.
const expectedEvents = [
    makeMongosEvent(-1, "insert"),
    makeMongosEvent(5, "insert"),
    makeMongosEvent(15, "insert"),
    makeMongosEvent(5, "delete", true),
    makeMongosEvent(5, "insert", true),
];
changeStreamTestMongos.assertNextChangesEqualUnordered({
    cursor: changeStreamMongos,
    expectedChanges: expectedEvents,
});

changeStreamTestMongos.assertNoChange(changeStreamMongos);
changeStreamTestMongos.cleanUp();

st.stop();
