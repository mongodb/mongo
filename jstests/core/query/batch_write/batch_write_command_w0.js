/**
 * Test unacknowledged write commands.
 *
 * This test creates secondary unique: true indexes without the shard key prefix.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_write_concern_unchanged,
 *   requires_non_retryable_writes,
 * ]
 */

function countEventually(collection, n) {
    assert.soon(
        function () {
            return collection.count() === n;
        },
        function () {
            return "unacknowledged write timed out";
        },
    );
}

let coll = db[jsTestName()];
coll.drop();

// EACH TEST BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
// Single document insert, w:0 write concern specified, missing ordered
coll.drop();
assert.commandWorked(db.runCommand({create: coll.getName()}));
let request = {insert: coll.getName(), documents: [{a: 1}], writeConcern: {w: 0}};
let result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 1);

//
// Single document upsert, write concern 0 specified, ordered = true
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {a: 1}}, upsert: true}],
    writeConcern: {w: 0},
    ordered: true,
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 1);

//
// Two document upsert, write concern 0 specified, ordered = true
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
request = {
    update: coll.getName(),
    updates: [
        {q: {a: 2}, u: {$set: {a: 1}}, upsert: true},
        {q: {a: 2}, u: {$set: {a: 2}}, upsert: true},
    ],
    writeConcern: {w: 0},
    ordered: true,
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 2);

//
// Upsert fail due to duplicate key index, w:0, ordered:true
assert(coll.drop());
assert.commandWorked(
    db.runCommand({createIndexes: coll.getName(), indexes: [{key: {a: 1}, name: "a_1", unique: true}]}),
);
request = {
    update: coll.getName(),
    updates: [
        {q: {b: 1}, u: {$set: {b: 1, a: 1}}, upsert: true},
        {q: {b: 2}, u: {$set: {b: 2, a: 1}}, upsert: true},
    ],
    writeConcern: {w: 0},
    ordered: true,
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 1);

//
// Single document delete, w:0 write concern specified
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 1}],
    writeConcern: {w: 0},
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 0);

//
// Cause remove error using ordered:false and w:0
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    delete: coll.getName(),
    deletes: [
        {q: {$set: {a: 1}}, limit: 0},
        {q: {$set: {a: 1}}, limit: 0},
        {q: {a: 1}, limit: 0},
    ],
    writeConcern: {w: 0},
    ordered: false,
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
countEventually(coll, 0);

//
// Cause remove error using ordered:true and w:0 - $set isn't a valid delete filter
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    delete: coll.getName(),
    deletes: [
        {q: {$set: {a: 1}}, limit: 0},
        {q: {$set: {a: 1}}, limit: 0},
        {q: {a: 1}, limit: 1},
    ],
    writeConcern: {w: 0},
    ordered: true,
};
result = coll.runCommand(request);
assert.eq({ok: 1}, result);
assert.eq(coll.count(), 1);

//
// When limit is not 0 and 1
assert(coll.drop());
assert.commandWorked(db.runCommand({create: coll.getName()}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 2}],
    writeConcern: {w: 0},
    ordered: false,
};
result = coll.runCommand(request);
// Unacknowledged writes are always OK
assert.eq({ok: 1}, result);
