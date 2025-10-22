/**
 * Tests that out-of-order keys are detected by validation with
 * collHash:true / revealHashedIds:[...]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];

assert.commandWorked(coll.createIndex({x: 1}));
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Without any corruption, vanilla collHash:true and revealHashedIds:[...] return fine.
let res = assert.commandWorked(coll.validate({collHash: true}));
assert(res.valid, res);
assert(res.all, res);
res = assert.commandWorked(coll.validate({collHash: true, revealHashedIds: "0123456789abcdef".split("")}));
assert(res.valid, res);
assert(res.revealedIds, res);

// When there is corruption, validate with collHash:true / revealHashedIds:[...] still reports it.
assert.commandWorked(db.adminCommand({configureFailPoint: "failRecordStoreTraversal", mode: "alwaysOn"}));
res = assert.commandWorked(coll.validate({collHash: true}));
assert(!res.valid, res);
assert(res.all, res);

res = assert.commandWorked(coll.validate({collHash: true, revealHashedIds: "0123456789abcdef".split("")}));
assert(!res.valid, res);
assert(res.revealedIds, res);

// Drill down doesn't run full validate.
res = assert.commandWorked(coll.validate({collHash: true, hashPrefixes: "0123456789abcdef".split("")}));
assert(res.valid, res);
assert(res.partial, res);

assert.commandWorked(db.adminCommand({configureFailPoint: "failRecordStoreTraversal", mode: "off"}));

MongoRunner.stopMongod(conn);
