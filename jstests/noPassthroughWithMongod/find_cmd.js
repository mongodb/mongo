// Basic tests for invoking the find command directly.

(function() {
const coll = db.findcmd;
const collname = coll.getName();
coll.drop();

let res;
let cursor;

// Non-existent collection.
res = coll.runCommand("find");
assert.commandWorked(res);
assert.eq(0, res.cursor.id);
assert.eq([], res.cursor.firstBatch);

// Empty collection.
assert.commandWorked(coll.getDB().createCollection(collname));
res = coll.runCommand("find");
assert.commandWorked(res);
assert.eq(0, res.cursor.id);
assert.eq([], res.cursor.firstBatch);

// Ensure find command keeps cursor open if tailing a capped collection.
coll.drop();
assert.commandWorked(coll.getDB().createCollection(collname, {capped: true, size: 2048}));
assert.commandWorked(coll.insert({_id: 1}));
res = coll.runCommand("find", {tailable: true});
assert.commandWorked(res);
assert.neq(0, res.cursor.id);
assert.eq([{_id: 1}], res.cursor.firstBatch);
coll.drop();

// Ensure that the tailable cursor is invalidated by running 'killCursors' after 'coll' has been
// dropped. This is done in order to ensure that the cursor doesn't timeout and modify the
// cursor serverStatus metrics that other tests may depend on (see BF-19234 for more details).
assert.commandWorked(db.runCommand({killCursors: collname, cursors: [res.cursor.id]}));

// Multiple batches.
for (let i = 0; i < 150; i++) {
    assert.commandWorked(coll.insert({_id: i}));
}
res = coll.runCommand("find", {filter: {_id: {$lt: 140}}});
assert.commandWorked(res);
assert.neq(0, res.cursor.id);
assert.eq(101, res.cursor.firstBatch.length);

cursor = new DBCommandCursor(coll.getDB(), res);
assert.eq(cursor.itcount(), 140);

// Command doesn't parse.
assert.commandFailed(coll.runCommand("find", {foo: "bar"}));

// Filter doesn't parse.
assert.commandFailed(coll.runCommand("find", {projection: {_id: 0}, filter: {$foo: "bar"}}));

// Special command namespace.
assert.commandFailed(coll.getDB().runCommand({find: "$cmd"}));
assert.commandFailed(coll.getDB().runCommand({find: "$cmd.sys.inprog"}));
})();
