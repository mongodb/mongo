/**
 * Tests user deletes on capped collections.
 */
(function() {
"use strict";

let replTest = new ReplSetTest({name: "capped_deletes", nodes: 2});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

let dbName = "test";
let collName = "capped_deletes";

let db = primary.getDB(dbName);

assert.commandWorked(db.createCollection(collName, {capped: true, size: 4096}));

let coll = db.getCollection(collName);

for (let i = 0; i < 10; i++) {
    coll.insert({_id: i});
}

let res = coll.runCommand("delete", {deletes: [{q: {_id: 2}, limit: 1}]});
assert.eq(coll.find().itcount(), 9, res);
assert.eq(res.n, 1, res);
assert.commandWorked(res);

res = coll.runCommand("delete", {deletes: [{q: {}, limit: 1, hint: {_id: 1}}]});
assert.eq(coll.find().itcount(), 8, res);
assert.eq(res.n, 1, res);
assert.commandWorked(res);

res = coll.runCommand("delete", {deletes: [{q: {_id: 5}, limit: 1, hint: {$natural: 1}}]});
assert.eq(coll.find().itcount(), 7, res);
assert.eq(res.n, 1, res);
assert.commandWorked(res);

res = coll.runCommand("delete", {deletes: [{q: {}, limit: 0}]});
assert.eq(coll.find().itcount(), 0, res);
assert.eq(res.n, 7, res);
assert.commandWorked(res);

replTest.stopSet();
}());
