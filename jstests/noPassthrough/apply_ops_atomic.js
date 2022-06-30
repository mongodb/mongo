/**
 * Validates atomic applyOps operations, including UUID propagation and replicating the actual
 * writes rather than the operations that were requested from the applyOps command.
 *
 * @tags: [
 *  requires_replication,
 * ]
 */

(function() {
'use strict';
load('jstests/libs/uuid_util.js');  // For getUUIDFromListCollections

const rst = new ReplSetTest({
    name: "apply_ops_atomic_test",
    nodes: 2,
});

rst.startSet();
rst.initiate();
rst.awaitNodesAgreeOnPrimary();

const conn = rst.getPrimary();
const db = conn.getDB("applyOpsAtomic");
const coll = db.getCollection('test');

assert.commandWorked(db.createCollection(coll.getName()));
const collUUID = getUUIDFromListCollections(db, coll.getName());

function validateInsertApplyOpsInOplog(conn, ns, uuid, id) {
    assert.eq(
        1,
        conn.getDB('local')
            .oplog.rs
            .find({
                'op': 'c',
                ns: 'admin.$cmd',
                'o.applyOps': {$elemMatch: {op: 'i', ns: ns, ui: uuid, 'o._id': id, 'o2._id': id}}
            })
            .itcount());
}

// Validates that an insert inside a nested applyOps succeeds.
{
    const collCountBefore = coll.find().itcount();
    const id = "InsertInNestedApplyOpsReturnsSuccess";
    assert.eq(0, coll.find({"_id": id}).itcount());
    const op = {
        applyOps: [{
            op: 'c',
            ns: 'admin.$cmd',
            o: {applyOps: [{op: 'i', ns: coll.getFullName(), ui: collUUID, o: {_id: id}}]}
        }]
    };
    assert.commandWorked(db.runCommand(op));
    assert.eq(1, coll.find({"_id": id}).itcount());
    assert.eq(collCountBefore + 1, coll.find().itcount());
    validateInsertApplyOpsInOplog(conn, coll.getFullName(), collUUID, id);
}

// Validates that an empty applyOps succeeds.
{
    const collCountBefore = coll.find().itcount();
    const latestApplyOpsInOplogTimestamp =
        db.getSiblingDB('local')
            .oplog.rs.find({ns: 'admin.$cmd', 'o.applyOps': {$exists: true}})
            .sort({$natural: -1})[0]
            .ts;
    assert.commandWorked(db.runCommand({applyOps: []}));
    assert.eq(collCountBefore, coll.find().itcount());
    // No applyOps is emitted.
    assert.eq(latestApplyOpsInOplogTimestamp,
              db.getSiblingDB('local')
                  .oplog.rs.find({ns: 'admin.$cmd', 'o.applyOps': {$exists: true}})
                  .sort({$natural: -1})[0]
                  .ts);
}

// Validates that an applyOps command that specifies the collection UUID replicates an applyOps
// entry that includes the collection UUID.
{
    const collCountBefore = coll.find().itcount();
    const id = "AtomicApplyOpsInsertWithUuidIntoCollectionWithUuid";
    assert.eq(0, coll.find({"_id": id}).itcount());
    const op = {applyOps: [{op: 'i', ns: coll.getFullName(), ui: collUUID, o: {_id: id}}]};
    assert.commandWorked(db.runCommand(op));
    assert.eq(1, coll.find({"_id": id}).itcount());
    assert.eq(collCountBefore + 1, coll.find().itcount());
    validateInsertApplyOpsInOplog(conn, coll.getFullName(), collUUID, id);
}

// Validates that an applyOps command that doesn't specify the collection UUID replicates an
// applyOps entry that includes the collection UUID.
{
    const collCountBefore = coll.find().itcount();
    const id = "AtomicApplyOpsInsertWithoutUuidIntoCollectionWithUuid";
    assert.eq(0, coll.find({"_id": id}).itcount());
    const op = {applyOps: [{op: 'i', ns: coll.getFullName(), o: {_id: id}}]};
    assert.commandWorked(db.runCommand(op));
    assert.eq(1, coll.find({"_id": id}).itcount());
    assert.eq(collCountBefore + 1, coll.find().itcount());
    validateInsertApplyOpsInOplog(conn, coll.getFullName(), collUUID, id);
}

// The applyOps command replicates what it wrote, not what it was asked to do. Validate that an
// update that results in an upsert replicates as an insert operation, not as an update as it was
// requested by the user via the applyOps command.
{
    const collCountBefore = coll.find().itcount();
    const idUnexisting = "unexistingDocument";
    const idUpserted = "upsertedDocument";
    assert.eq(0, coll.find({"_id": idUnexisting}).itcount());
    assert.eq(0, coll.find({"_id": idUpserted}).itcount());
    const op = {
        applyOps: [{
            op: 'u',
            ns: coll.getFullName(),
            o2: {_id: idUnexisting},
            o: {$v: 2, diff: {u: {_id: idUpserted}}}
        }]
    };
    assert.commandWorked(db.runCommand(op));
    assert.eq(0, coll.find({"_id": idUnexisting}).itcount());
    assert.eq(1, coll.find({"_id": idUpserted}).itcount());
    assert.eq(collCountBefore + 1, coll.find().itcount());
    validateInsertApplyOpsInOplog(conn, coll.getFullName(), collUUID, idUpserted);
}

// applyOps to a collection with pre-images enabled falls back to non-atomic mode.
{
    const preImageCollName = 'c';
    const coll = db.getCollection(preImageCollName);
    assert.commandWorked(
        db.createCollection(preImageCollName, {changeStreamPreAndPostImages: {enabled: true}}));
    assert.eq(0, coll.find().itcount());
    assert.commandWorked(db.c.insertOne({_id: 1, a: "a"}));
    assert.eq(1, coll.find({_id: 1, a: "a"}).itcount());
    assert.commandWorked(db.runCommand({
        applyOps: [{
            op: 'u',
            ns: coll.getFullName(),
            o2: {_id: 1},
            o: {$v: 2, diff: {u: {a: "b"}}},
        }]
    }));
    assert.eq(1, coll.find().itcount());
    assert.eq(1, coll.find({_id: 1, a: "b"}).itcount());
    assert.eq(
        1,
        conn.getDB('local')
            .oplog.rs.find({'op': 'u', ns: coll.getFullName(), 'o.diff.u.a': 'b', 'o2._id': 1})
            .itcount());
}

rst.stopSet();
})();
