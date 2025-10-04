// @tags: [
//   assumes_write_concern_unchanged,
// ]

// check that shell writeconcern work correctly
// 1.) tests that it can be set on each level and is inherited
// 2.) tests that each operation (update/insert/remove/save) take and ensure a write concern

let collA = db.shell_wc_a;
let collB = db.shell_wc_b;
collA.drop();
collB.drop();

// test inheritance
db.setWriteConcern({w: 1});
assert.eq(1, db.getWriteConcern().toJSON().w);
assert.eq(1, collB.getWriteConcern().toJSON().w);

collA.setWriteConcern({w: 2});
assert.eq(2, collA.getWriteConcern().toJSON().w);
collA.unsetWriteConcern();
assert.eq(1, collA.getWriteConcern().toJSON().w);

db.unsetWriteConcern();
assert.eq(undefined, collA.getWriteConcern());
assert.eq(undefined, collB.getWriteConcern());
assert.eq(undefined, db.getWriteConcern());

// test methods, by generating an error
var res = assert.commandWorked(collA.save({_id: 1}, {writeConcern: {w: 1}}));
assert.eq(1, res.nUpserted, tojson(res));

var res = assert.commandWorked(collA.update({_id: 1}, {_id: 1}, {writeConcern: {w: 1}}));
assert.eq(1, res.nMatched, tojson(res));

var res = assert.commandWorked(collA.update({_id: 1}, {_id: 1}, {writeConcern: {w: 1}}));
assert.eq(1, res.nMatched, tojson(res));

var res = assert.commandWorked(collA.insert({_id: 2}, {writeConcern: {w: 1}}));
assert.eq(1, res.nInserted, tojson(res));

var res = assert.commandWorked(collA.remove({_id: 3}, {writeConcern: {w: 1}}));
assert.eq(0, res.nRemoved, tojson(res));

var res = assert.commandWorked(collA.remove({_id: 1}, {writeConcern: {w: 1}}));
assert.eq(1, res.nRemoved, tojson(res));

// Test ordered write concern, and that the write concern isn't run/error.
assert.commandWorked(collA.insert({_id: 1}));

var res = assert.writeError(collA.insert([{_id: 1}, {_id: 1}], {ordered: true, writeConcern: {w: 1}}));
assert.eq(1, res.getWriteErrors().length, tojson(res));
assert.eq(undefined, res.writeConcernErrors, tojson(res));

var res = assert.writeError(collA.insert([{_id: 1}, {_id: 1}], {ordered: false, writeConcern: {w: 1}}));
assert.eq(2, res.getWriteErrors().length, tojson(res));
assert.eq(undefined, res.writeConcernErrors, tojson(res));
