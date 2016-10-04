//
// Ensures that mongod respects the batch write protocols for delete
//

var coll = db.getCollection("batch_write_delete");
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
coll.insert({a: 1});
request = {
    delete: coll.getName()
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
assert.eq(1, coll.count());

//
// Single document remove, default write concern specified
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 1}]
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(0, coll.count());

//
// Single document delete, w:0 write concern specified
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 1}],
    writeConcern: {w: 0}
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(0, coll.count());

for (var field in result) {
    assert.eq('ok', field, 'unexpected field found in result: ' + field);
}

//
// Single document remove, w:1 write concern specified, ordered:true
coll.remove({});
coll.insert([{a: 1}, {a: 1}]);
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 1}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Multiple document remove, w:1 write concern specified, ordered:true, default top
coll.remove({});
coll.insert([{a: 1}, {a: 1}]);
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 0}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(2, result.n);
assert.eq(0, coll.count());

//
// Multiple document remove, w:1 write concern specified, ordered:true, top:0
coll.remove({});
coll.insert([{a: 1}, {a: 1}]);
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 0}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(2, result.n);
assert.eq(0, coll.count());

//
// Large batch under the size threshold should delete successfully
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize; ++i) {
    coll.insert({a: i});
    batch.push({q: {a: i}, limit: 0});
}
request = {
    delete: coll.getName(),
    deletes: batch,
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultOK(result), tojson(result));
assert.eq(batch.length, result.n);
assert.eq(0, coll.count());

//
// Large batch above the size threshold should fail to delete
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize + 1; ++i) {
    coll.insert({a: i});
    batch.push({q: {a: i}, limit: 0});
}
request = {
    delete: coll.getName(),
    deletes: batch,
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
assert.eq(batch.length, coll.count());

//
// Cause remove error using ordered:true
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 0}, {q: {$set: {a: 1}}, limit: 0}, {q: {$set: {a: 1}}, limit: 0}],
    writeConcern: {w: 1},
    ordered: true
};
result = coll.runCommand(request);
assert.commandWorked(result);
assert.eq(1, result.n);
assert(result.writeErrors != null);
assert.eq(1, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);
assert.eq(0, coll.count());

//
// Cause remove error using ordered:false
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {$set: {a: 1}}, limit: 0}, {q: {$set: {a: 1}}, limit: 0}, {q: {a: 1}, limit: 0}],
    writeConcern: {w: 1},
    ordered: false
};
result = coll.runCommand(request);
assert.commandWorked(result);
assert.eq(1, result.n);
assert.eq(2, result.writeErrors.length);

assert.eq(0, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);

assert.eq(1, result.writeErrors[1].index);
assert.eq('number', typeof result.writeErrors[1].code);
assert.eq('string', typeof result.writeErrors[1].errmsg);
assert.eq(0, coll.count());

//
// Cause remove error using ordered:false and w:0
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {$set: {a: 1}}, limit: 0}, {q: {$set: {a: 1}}, limit: 0}, {q: {a: 1}, limit: 0}],
    writeConcern: {w: 0},
    ordered: false
};
result = coll.runCommand(request);
assert.commandWorked(result);
assert.eq(0, coll.count());

for (var field in result) {
    assert.eq('ok', field, 'unexpected field found in result: ' + field);
}

//
// Cause remove error using ordered:true and w:0
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes:
        [{q: {$set: {a: 1}}, limit: 0}, {q: {$set: {a: 1}}, limit: 0}, {q: {a: 1}, limit: (1)}],
    writeConcern: {w: 0},
    ordered: true
};
result = coll.runCommand(request);
assert.commandWorked(result);
assert.eq(1, coll.count());

for (var field in result) {
    assert.eq('ok', field, 'unexpected field found in result: ' + field);
}

//
// When limit is not 0 and 1
coll.remove({});
coll.insert({a: 1});
request = {
    delete: coll.getName(),
    deletes: [{q: {a: 1}, limit: 2}],
    writeConcern: {w: 0},
    ordered: false
};
result = coll.runCommand(request);
assert(resultNOK(result), tojson(result));
