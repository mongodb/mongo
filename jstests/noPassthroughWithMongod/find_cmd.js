// Basic tests for invoking the find command directly.

var coll = db.findcmd;
var collname = coll.getName();
coll.drop();

var res;
var cursor;

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
assert.writeOK(coll.insert({_id: 1}));
res = coll.runCommand("find", {tailable: true});
assert.commandWorked(res);
assert.neq(0, res.cursor.id);
assert.eq([{_id: 1}], res.cursor.firstBatch);

// Multiple batches.
coll.drop();
for (var i = 0; i < 150; i++) {
    assert.writeOK(coll.insert({_id: i}));
}
res = coll.runCommand("find", {filter: {_id: {$lt: 140}}});
assert.commandWorked(res);
assert.neq(0, res.cursor.id);
assert.eq(101, res.cursor.firstBatch.length);

cursor = new DBCommandCursor(coll.getDB().getMongo(), res);
assert.eq(cursor.itcount(), 140);

// Command doesn't parse.
assert.commandFailed(coll.runCommand("find", {foo: "bar"}));

// Filter doesn't parse.
assert.commandFailed(coll.runCommand("find", {projection: {_id: 0}, filter: {$foo: "bar"}}));

// Special command namespace.
assert.commandFailed(coll.getDB().runCommand({find: "$cmd"}));
assert.commandFailed(coll.getDB().runCommand({find: "$cmd.sys.inprog"}));