// Cannot implicitly shard accessed collections because of not being able to create unique index
// using hashed shard key pattern.
//
// @tags: [
//   assumes_write_concern_unchanged,
//   cannot_create_unique_index_when_using_hashed_shard_key,
//   requires_fastcount,
//
//   # Uses index building in background
//   requires_background_index,
//   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer
//   assumes_balancer_off,
// ]
//
//
// Ensures that mongod respects the batch write protocol for inserts
//

let coll = db.getCollection("batch_write_insert");
coll.drop();

let request;
let result;
let batch;

let maxWriteBatchSize = db.hello().maxWriteBatchSize;

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
coll.drop();
request = {
    insert: coll.getName(),
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));

//
// Single document insert, no write concern specified
coll.drop();
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:1 write concern specified, ordered:true
coll.drop();
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 1},
    ordered: true,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:1 write concern specified, ordered:false
coll.drop();
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Document with valid nested key should insert (op log format)
coll.drop();
request = {
    insert: coll.getName(),
    documents: [{o: {$set: {a: 1}}}],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Large batch under the size threshold should insert successfully
coll.drop();
batch = [];
for (var i = 0; i < maxWriteBatchSize; ++i) {
    batch.push({});
}
request = {
    insert: coll.getName(),
    documents: batch,
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(batch.length, result.n);
assert.eq(coll.count(), batch.length);

//
// Large batch above the size threshold should fail to insert
coll.drop();
batch = [];
for (var i = 0; i < maxWriteBatchSize + 1; ++i) {
    batch.push({});
}
request = {
    insert: coll.getName(),
    documents: batch,
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
assert.eq(coll.count(), 0);

//
// Batch of size zero should fail to insert
coll.drop();
request = {
    insert: coll.getName(),
    documents: [],
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));

//
//
// Unique index tests

//
// Should fail single insert due to duplicate key
coll.drop();
coll.createIndex({a: 1}, {unique: true});
coll.insert({a: 1});
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.writeErrors.length);
assert.eq(0, result.n);
assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered false
coll.drop();
coll.createIndex({a: 1}, {unique: true});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 1}, {a: 1}],
    writeConcern: {w: 1},
    ordered: false,
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
assert.eq(2, result.writeErrors.length);
assert.eq(coll.count(), 1);

assert.eq(1, result.writeErrors[0].index);
assert.eq("number", typeof result.writeErrors[0].code);
assert.eq("string", typeof result.writeErrors[0].errmsg);

assert.eq(2, result.writeErrors[1].index);
assert.eq("number", typeof result.writeErrors[1].code);
assert.eq("string", typeof result.writeErrors[1].errmsg);

assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered true
coll.drop();
coll.createIndex({a: 1}, {unique: true});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 1}, {a: 1}],
    writeConcern: {w: 1},
    ordered: true,
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
assert.eq(1, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq("number", typeof result.writeErrors[0].code);
assert.eq("string", typeof result.writeErrors[0].errmsg);

assert.eq(coll.count(), 1);

//
// Ensure _id is the first field in all documents
coll.drop();
coll.createIndex({a: 1}, {unique: true});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 2, _id: 2}],
};
result = coll.runCommand(request);
assert.eq(2, coll.count());
coll.find().forEach(function (doc) {
    let firstKey = null;
    for (let key in doc) {
        firstKey = key;
        break;
    }
    assert.eq("_id", firstKey, tojson(doc));
});

//
// Ensure we error out correctly in the middle of a batch
coll.drop();
coll.insert({_id: 50}); // Create a document to force a duplicate key exception.

let bulk = coll.initializeOrderedBulkOp();
for (i = 1; i < 100; i++) {
    bulk.insert({_id: i});
}
try {
    bulk.execute();
    assert(false, "should have failed due to duplicate key");
} catch (err) {
    assert(coll.count() == 50, "Unexpected number inserted by bulk write: " + coll.count());
}
