/**
 * Tests that batched multi-deletes can span multiple oplog entries.
 *
 * This is done by constraining the number of write operations contained in
 * each replicated applyOps oplog entry to show how "large" batched writes are
 * handled by the primary.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {votes: 0, priority: 0}},
    ]
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

const docIds = [0, 1, 2, 3];
assert.commandWorked(coll.insert(docIds.map((x) => {
    return {_id: x, x: x};
})));

// Set up server to split deletes over multiple oplog entries
// such that each oplog entry contains two delete operations.
const result = assert.commandWorked(coll.remove({}));
jsTestLog('delete result: ' + tojson(result));
assert.eq(result.nRemoved, docIds.length);
assert.eq(coll.countDocuments({}), 0);

// Check oplog entries.
const entries =
    rst.findOplog(primary, {ns: 'admin.$cmd', 'o.applyOps.0.ns': coll.getFullName()}).toArray();
jsTestLog('applyOps oplog entries: ' + tojson(entries));
assert.eq(entries.length, 1);
let entry = entries[0];
assert.eq(entry.ns, 'admin.$cmd', tojson(entry));
assert(entry.o.hasOwnProperty('applyOps'), tojson(entry));
assert.eq(entry.o.applyOps.length, 4, tojson(entry));
entry.o.applyOps.forEach((op) => {
    assert.eq(op.op, 'd', tojson(op));
    assert.eq(op.ns, coll.getFullName(), tojson(op));
    const idVal = op.o._id;
    assert.neq(docIds.indexOf(idVal), -1, tojson(op));
});

rst.stopSet();
})();
