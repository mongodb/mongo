/**
 * Tests that write oplog entries contain the correct document key on sharded and non-sharded
 * collection, with and without transaction.
 *
 *  @tags: [
 *    requires_fcv_53,
 *    requires_sharding,
 *    uses_transactions,
 *  ]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

const mongos = st.s;
const dbName = jsTestName();
const db = mongos.getDB(dbName);

Array.prototype.flatMap = function(lambda) {
    return Array.prototype.concat.apply([], this.map(lambda));
};

const testWriteOplogDocumentKey = ({sharded, inTransaction}) => {
    const shardKey = {a: 1};
    const doc0 = {_id: 0, a: 0, b: 0};
    const doc1 = {_id: 0, a: 0, b: 1};
    const doc2 = {_id: 0, a: 0, b: 2};
    const docKeys = {
        sharded: {a: 0, _id: 0},
        unsharded: {_id: 0},
    };
    const collName = `testcase-${sharded}-${inTransaction}`;
    const ns = `${dbName}.${collName}`;
    assert.commandWorked(db.createCollection(collName));
    if (sharded) {
        assert.commandWorked(db.adminCommand({enableSharding: dbName}));
        assert.commandWorked(db.getCollection(collName).createIndex(shardKey));
        assert.commandWorked(db.adminCommand({shardCollection: ns, key: shardKey}));
    }

    const performWrites = (writeFunc) => {
        if (inTransaction) {
            const session = mongos.startSession();
            const sessionDB = session.getDatabase(dbName);
            const sessionColl = sessionDB.getCollection(collName);
            session.startTransaction();
            writeFunc(sessionColl);
            session.commitTransaction_forTesting();
        } else {
            const coll = db.getCollection(collName);
            writeFunc(coll);
        }
    };
    performWrites(function basicWriteOps(coll) {
        assert.commandWorked(coll.insert(doc0));
        assert.commandWorked(coll.replaceOne({_id: 0}, doc1));
        assert.commandWorked(coll.updateOne({_id: 0}, {$set: doc2}));
        assert.commandWorked(coll.deleteOne({_id: 0}));
    });

    const primary = st.getPrimaryShard(dbName);
    const query = inTransaction ? {'o.applyOps.0.ns': ns} : {ns, op: {$in: ['i', 'u', 'd']}};
    let oplogs = primary.getDB("local").oplog.rs.find(query).sort({$natural: 1}).toArray();
    if (inTransaction) {
        oplogs = oplogs.flatMap(oplog => oplog.o.applyOps);
    }

    assert.eq(oplogs.length, 4, tojson(oplogs));
    const [insertOplog, replaceOplog, updateOplog, deleteOplog] = oplogs;
    const docKey = sharded ? docKeys.sharded : docKeys.unsharded;
    assert.eq(insertOplog.op, 'i', insertOplog);
    assert.docEq(doc0, insertOplog.o, insertOplog);
    assert.docEq(docKey, insertOplog.o2, insertOplog);

    assert.eq(replaceOplog.op, 'u', replaceOplog);
    assert.docEq(doc1, replaceOplog.o, replaceOplog);
    assert.docEq(docKey, replaceOplog.o2, replaceOplog);

    assert.eq(updateOplog.op, 'u', updateOplog);
    assert.docEq(docKey, updateOplog.o2, updateOplog);

    assert.eq(deleteOplog.op, 'd', deleteOplog);
    assert.docEq(docKey, deleteOplog.o, deleteOplog);

    performWrites(function largeInsert(coll) {
        const largeDoc = {_id: 'x'.repeat(16 * 1024 * 1024), a: 0};
        assert.commandFailedWithCode(coll.insert(largeDoc), ErrorCodes.BadValue);
    });
};

testWriteOplogDocumentKey({sharded: false, inTransaction: false});
testWriteOplogDocumentKey({sharded: false, inTransaction: true});
testWriteOplogDocumentKey({sharded: true, inTransaction: false});
testWriteOplogDocumentKey({sharded: true, inTransaction: true});

st.stop();
}());
