/**
 * Tests that DDL operations can happen against a collection being validated with {background:
 * true}, which runs lock-free.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */

// Disable the checkpoint thread.
const rst = new ReplSetTest({nodes: 1, nodeOptions: {syncdelay: 0}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const collName = jsTestName();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

// Validate errors if the collection doesn't exist.
(function() {
assert.commandFailedWithCode(db.runCommand({validate: collName, background: false}),
                             ErrorCodes.NamespaceNotFound);
assert.commandFailedWithCode(db.runCommand({validate: collName, background: true}),
                             ErrorCodes.NamespaceNotFound);
}());

// Validate with {background: true} fails to find an uncheckpoint'ed collection.
(function() {
assert.commandWorked(db.createCollection(collName));
let res = assert.commandWorked(db.runCommand({validate: collName, background: false}));
assert(res.valid);

assert.commandFailedWithCode(db.runCommand({validate: collName, background: true}),
                             ErrorCodes.NamespaceNotFound);

coll.drop();
assert.commandWorked(db.adminCommand({fsync: 1}));
}());

// Validate with {background: true} skips validating indexes that are not part of the same
// checkpoint that the collection is.
(function() {
assert.commandWorked(db.createCollection(collName));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(db.adminCommand({fsync: 1}));

assert.commandWorked(coll.createIndex({c: 1}));
assert.commandWorked(coll.createIndex({d: 1}));

let res = assert.commandWorked(db.runCommand({validate: collName, background: false}));
assert(res.valid);
assert.eq(5, res.nIndexes);

res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(res.valid);
assert.eq(3, res.nIndexes);

assert.commandWorked(db.adminCommand({fsync: 1}));

res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(res.valid);
assert.eq(5, res.nIndexes);

coll.drop();
assert.commandWorked(db.adminCommand({fsync: 1}));
}());

// Validate with {background: true} validates indexes that are dropped but still part of the
// checkpoint.
(function() {
assert.commandWorked(db.createCollection(collName));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(db.adminCommand({fsync: 1}));

let res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(res.valid);
assert.eq(3, res.nIndexes);

assert.commandWorked(coll.dropIndex({a: 1}));
assert.commandWorked(coll.dropIndex({b: 1}));

res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(res.valid);
assert.eq(3, res.nIndexes);

assert.commandWorked(db.adminCommand({fsync: 1}));

res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(res.valid);
assert.eq(1, res.nIndexes);
}());

rst.stopSet();
