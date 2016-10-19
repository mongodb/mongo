//
// Ensures that mongod respects the batch write protocol for inserts
//

load("jstests/libs/get_index_helpers.js");

var coll = db.getCollection("batch_write_insert");
coll.drop();

assert(coll.getDB().getMongo().useWriteCommands(), "test is not running with write commands");

var request;
var result;
var batch;

var maxWriteBatchSize = 1000;

function resultOK(result) {
    return result.ok && !('code' in result) && !('errmsg' in result) && !('errInfo' in result) &&
        !('writeErrors' in result);
}

function resultNOK(result) {
    return !result.ok && typeof(result.code) == 'number' && typeof(result.errmsg) == 'string';
}

// EACH TEST BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
// NO DOCS, illegal command
coll.remove({});
request = {
    insert: coll.getName()
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));

//
// Single document insert, no write concern specified
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}]
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:0 write concern specified, missing ordered
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 0}
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(coll.count(), 1);

for (var field in result) {
    assert.eq('ok', field, 'unexpected field found in result: ' + field);
}

//
// Single document insert, w:1 write concern specified, ordered:true
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 1},
    ordered: true
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:1 write concern specified, ordered:false
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Document with illegal key should fail
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{$set: {a: 1}}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert(result.writeErrors != null);
assert.eq(1, result.writeErrors.length);
assert.eq(0, result.n);
assert.eq(coll.count(), 0);

//
// Document with valid nested key should insert (op log format)
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{o: {$set: {a: 1}}}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Large batch under the size threshold should insert successfully
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize; ++i) {
    batch.push({});
}
request = {
    insert: coll.getName(),
    documents: batch,
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(batch.length, result.n);
assert.eq(coll.count(), batch.length);

//
// Large batch above the size threshold should fail to insert
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize + 1; ++i) {
    batch.push({});
}
request = {
    insert: coll.getName(),
    documents: batch,
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
assert.eq(coll.count(), 0);

//
// Batch of size zero should fail to insert
coll.remove({});
request = {
    insert: coll.getName(),
    documents: []
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));

//
//
// Unique index tests
coll.remove({});
coll.ensureIndex({a: 1}, {unique: true});

//
// Should fail single insert due to duplicate key
coll.remove({});
coll.insert({a: 1});
request = {
    insert: coll.getName(),
    documents: [{a: 1}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.writeErrors.length);
assert.eq(0, result.n);
assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered false
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 1}, {a: 1}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
assert.eq(2, result.writeErrors.length);
assert.eq(coll.count(), 1);

assert.eq(1, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);

assert.eq(2, result.writeErrors[1].index);
assert.eq('number', typeof result.writeErrors[1].code);
assert.eq('string', typeof result.writeErrors[1].errmsg);

assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered true
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 1}, {a: 1}],
    writeConcern: {w: 1},
    ordered: true
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
assert.eq(1, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);

assert.eq(coll.count(), 1);

//
// Ensure _id is the first field in all documents
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{a: 1}, {a: 2, _id: 2}]
};
result = coll.runCommand(request);
assert.eq(2, coll.count());
coll.find().forEach(function(doc) {
    var firstKey = null;
    for (var key in doc) {
        firstKey = key;
        break;
    }
    assert.eq("_id", firstKey, tojson(doc));
});

//
//
// Index insertion tests - currently supported via bulk write commands

//
// Successful index creation
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), key: {x: 1}, name: "x_1"}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
var allIndexes = coll.getIndexes();
var spec = GetIndexHelpers.findByName(allIndexes, "x_1");
assert.neq(null, spec, "Index with name 'x_1' not found: " + tojson(allIndexes));
assert.lte(2, spec.v, tojson(spec));

// Test that a v=1 index can be created by inserting into the system.indexes collection.
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), key: {x: 1}, name: "x_1", v: 1}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
allIndexes = coll.getIndexes();
spec = GetIndexHelpers.findByName(allIndexes, "x_1");
assert.neq(null, spec, "Index with name 'x_1' not found: " + tojson(allIndexes));
assert.eq(1, spec.v, tojson(spec));

//
// Duplicate index insertion gives n = 0
coll.drop();
coll.ensureIndex({x: 1}, {unique: true});
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), key: {x: 1}, name: "x_1", unique: true}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(0, result.n, 'duplicate index insertion should give n = 0: ' + tojson(result));
assert(!('writeErrors' in result));
assert.eq(coll.getIndexes().length, 2);

//
// Invalid index insertion with mismatched collection db
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: "invalid." + coll.getName(), key: {x: 1}, name: "x_1", unique: true}]
};
result = coll.runCommand(request);
assert(!result.ok || (result.n == 0 && result.writeErrors.length == 1), tojson(result));
assert.eq(coll.getIndexes().length, 0);

//
// Empty index insertion
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{}]
};
result = coll.runCommand(request);
assert(!result.ok || (result.n == 0 && result.writeErrors.length == 1), tojson(result));
assert.eq(coll.getIndexes().length, 0);

//
// Invalid index desc
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString()}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 0);

//
// Invalid index desc
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), key: {x: 1}}]
};
result = coll.runCommand(request);
print(tojson(result));
assert(result.ok, tojson(result));
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 0);

//
// Invalid index desc
coll.drop();
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), name: "x_1"}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 0);

//
// Cannot insert more than one index at a time through the batch writes
coll.drop();
request = {
    insert: "system.indexes",
    documents: [
        {ns: coll.toString(), key: {x: 1}, name: "x_1"},
        {ns: coll.toString(), key: {y: 1}, name: "y_1"}
    ]
};
result = coll.runCommand(request);
assert(!result.ok, tojson(result));
assert.eq(coll.getIndexes().length, 0);

//
// Ensure we error out correctly in the middle of a batch
coll.drop();
coll.insert({_id: 50});  // Create a document to force a duplicate key exception.

var bulk = coll.initializeOrderedBulkOp();
for (i = 1; i < 100; i++) {
    bulk.insert({_id: i});
}
try {
    bulk.execute();
    assert(false, "should have failed due to duplicate key");
} catch (err) {
    assert(coll.count() == 50, "Unexpected number inserted by bulk write: " + coll.count());
}

//
// Background index creation
// Note: due to SERVER-13304 this test is at the end of this file, and we don't drop
// the collection afterwards.
coll.drop();
coll.insert({x: 1});
request = {
    insert: "system.indexes",
    documents: [{ns: coll.toString(), key: {x: 1}, name: "x_1", background: true}]
};
result = coll.runCommand(request);
assert(result.ok, tojson(result));
assert.eq(1, result.n);
allIndexes = coll.getIndexes();
spec = GetIndexHelpers.findByName(allIndexes, "x_1");
assert.neq(null, spec, "Index with name 'x_1' not found: " + tojson(allIndexes));
assert.lte(2, spec.v, tojson(spec));
