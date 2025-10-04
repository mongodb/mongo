// @tags: [
//   # This test creates secondary unique: true indexes without the shard key prefix.
//   assumes_unsharded_collection,
//   assumes_write_concern_unchanged,
//   requires_multi_updates,
//   requires_non_retryable_writes,
//   requires_fastcount,
//   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer
//   assumes_balancer_off,
// ]

//
// Ensures that mongod respects the batch write protocols for updates
//

const coll = db[jsTestName()];
coll.drop();

let request;
let result;
let batch;

const maxWriteBatchSize = db.hello().maxWriteBatchSize;

function resultOK(result) {
    return (
        result.ok &&
        !("code" in result) &&
        !("errmsg" in result) &&
        !("errInfo" in result) &&
        !("writeErrors" in result)
    );
}

function resultNOK(result) {
    return !result.ok && typeof result.code == "number" && typeof result.errmsg == "string";
}

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

// EACH TEST BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
// NO DOCS, illegal command
coll.remove({});
request = {
    update: coll.getName(),
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));

//
// Single document upsert, no write concern specified
coll.remove({});
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {a: 1}}, upsert: true}],
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert("upserted" in result);
assert.eq(1, result.upserted.length);
assert.eq(0, result.upserted[0].index);

// Count the upserted doc
let upsertedId = result.upserted[0]._id;
assert.eq(1, coll.count({_id: upsertedId}));
assert.eq(0, result.nModified, "missing/wrong nModified");

//
// Single document upsert, write concern specified, no ordered specified
assert.commandWorked(coll.remove({}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {a: 1}}, upsert: true}],
    writeConcern: {w: 1},
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert("upserted" in result);
assert.eq(1, result.upserted.length);
assert.eq(0, result.upserted[0].index);

// Count the upserted doc
upsertedId = result.upserted[0]._id;
assert.eq(1, coll.count({_id: upsertedId}));
assert.eq(0, result.nModified, "missing/wrong nModified");

//
// Single document upsert, write concern specified, ordered = true
assert.commandWorked(coll.remove({}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {a: 1}}, upsert: true}],
    writeConcern: {w: 1},
    ordered: true,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert("upserted" in result);
assert.eq(1, result.upserted.length);
assert.eq(0, result.upserted[0].index);

// Count the upserted doc
upsertedId = result.upserted[0]._id;
assert.eq(1, coll.count({_id: upsertedId}));
assert.eq(0, result.nModified, "missing/wrong nModified");

//
// Single document update
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {c: 1}}}],
    writeConcern: {w: 1},
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert(!("upserted" in result));
assert.eq(1, coll.count());
assert.eq(1, result.nModified, "missing/wrong nModified");

//
// Multi document update/upsert
assert(coll.remove({}));
assert.commandWorked(coll.insert({b: 1}));
request = {
    update: coll.getName(),
    updates: [
        {q: {b: 1}, u: {$set: {b: 1, a: 1}}, upsert: true},
        {q: {b: 2}, u: {$set: {b: 2, a: 1}}, upsert: true},
    ],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(2, result.n);
assert.eq(1, result.nModified, "missing/wrong nModified");

assert.eq(1, result.upserted.length);
assert.eq(1, result.upserted[0].index);
assert.eq(1, coll.count({_id: result.upserted[0]._id}));
assert.eq(2, coll.count());

//
// Multiple document update
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {c: 2}}, multi: true}],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(2, result.n);
assert.eq(2, result.nModified, "missing/wrong nModified");
assert.eq(2, coll.find({a: 1, c: 2}).count());
assert.eq(2, coll.count());

//
// Multiple document update, some no-ops
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({a: 1, c: 2}));
assert.commandWorked(coll.insert({a: 1}));
request = {
    update: coll.getName(),
    updates: [{q: {a: 1}, u: {$set: {c: 2}}, multi: true}],
    writeConcern: {w: 1},
    ordered: false,
};
printjson((result = coll.runCommand(request)));
assert(resultOK(result), tojson(result));
assert.eq(2, result.n);
assert.eq(1, result.nModified, "missing/wrong nModified");
assert.eq(2, coll.find({a: 1, c: 2}).count());
assert.eq(2, coll.count());

//
// Large batch under the size threshold should update successfully
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({a: 0}));
batch = [];
for (let i = 0; i < maxWriteBatchSize; ++i) {
    batch.push({q: {}, u: {$inc: {a: 1}}});
}
request = {
    update: coll.getName(),
    updates: batch,
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(batch.length, result.n);
assert.eq(batch.length, result.nModified, "missing/wrong nModified");
assert.eq(1, coll.find({a: batch.length}).count());
assert.eq(1, coll.count());

//
// Large batch above the size threshold should fail to update
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({a: 0}));
batch = [];
for (let i = 0; i < maxWriteBatchSize + 1; ++i) {
    batch.push({q: {}, u: {$inc: {a: 1}}});
}
request = {
    update: coll.getName(),
    updates: batch,
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
assert.eq(1, coll.find({a: 0}).count());
assert.eq(1, coll.count());

//
//
// Unique index tests

//
// Upsert fail due to duplicate key index, w:1, ordered:true
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
request = {
    update: coll.getName(),
    updates: [
        {q: {b: 1}, u: {$set: {b: 1, a: 1}}, upsert: true},
        {q: {b: 3}, u: {$set: {b: 3, a: 2}}, upsert: true},
        {q: {b: 2}, u: {$set: {b: 2, a: 1}}, upsert: true},
    ],
    writeConcern: {w: 1},
    ordered: true,
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(2, result.n);
assert.eq(0, result.nModified, "wrong nModified");
assert.eq(1, result.writeErrors.length);

assert.eq(2, result.writeErrors[0].index);
assert.eq("number", typeof result.writeErrors[0].code);
assert.eq("string", typeof result.writeErrors[0].errmsg);

assert.eq(2, result.upserted.length);
assert.eq(0, result.upserted[0].index);
assert.eq(1, coll.count({_id: result.upserted[0]._id}));

assert.eq(1, result.upserted[1].index);
assert.eq(1, coll.count({_id: result.upserted[1]._id}));

//
// Upsert fail due to duplicate key index, w:1, ordered:false
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
request = {
    update: coll.getName(),
    updates: [
        {q: {b: 1}, u: {$set: {b: 1, a: 1}}, upsert: true},
        {q: {b: 2}, u: {$set: {b: 2, a: 1}}, upsert: true},
        {q: {b: 2}, u: {$set: {b: 2, a: 1}}, upsert: true},
        {q: {b: 3}, u: {$set: {b: 3, a: 3}}, upsert: true},
    ],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(2, result.n);
assert.eq(0, result.nModified, "wrong nModified");
assert.eq(2, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq("number", typeof result.writeErrors[0].code);
assert.eq("string", typeof result.writeErrors[0].errmsg);

assert.eq(2, result.writeErrors[1].index);
assert.eq("number", typeof result.writeErrors[1].code);
assert.eq("string", typeof result.writeErrors[1].errmsg);

assert.eq(2, result.upserted.length);
assert.eq(0, result.upserted[0].index);
assert.eq(1, coll.count({_id: result.upserted[0]._id}));

assert.eq(3, result.upserted[1].index);
assert.eq(1, coll.count({_id: result.upserted[1]._id}));
