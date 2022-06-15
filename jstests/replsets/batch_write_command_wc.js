// Tests write-concern-related batch write protocol functionality
(function() {

// Skip this test when running with storage engines other than inMemory, as the test relies on
// journaling not being active.
if (jsTest.options().storageEngine !== "inMemory") {
    jsTest.log("Skipping test because it is only applicable for the inMemory storage engine");
    return;
}

var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

jsTest.log("Starting no journal/repl set tests...");

// Start a single-node replica set with no journal
// Allows testing immediate write concern failures and wc application failures
var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
var mongod = rst.getPrimary();
var coll = mongod.getCollection("test.batch_write_command_wc");

//
// Basic insert, default WC
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}]
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.find().itcount());

//
// Basic insert, majority WC
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 'majority'}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.find().itcount());

//
// Basic insert,  w:2 WC
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 2}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.find().itcount());

//
// Basic insert, immediate nojournal error
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {j: true}
});
printjson(result = coll.runCommand(request));
assert(!result.ok);
assert.eq(0, coll.find().itcount());

//
// Basic insert, timeout wc error
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 3, wtimeout: 1}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError);
assert.eq(100, result.writeConcernError.code);
assert.eq(1, coll.find().itcount());

//
// Basic insert, wmode wc error
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 'invalid'}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Two ordered inserts, write error and wc error both reported
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}, {_id: /a/}],
    writeConcern: {w: 'invalid'}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Two unordered inserts, write error and wc error reported
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}, {_id: /a/}],
    writeConcern: {w: 'invalid'},
    ordered: false
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert(result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Write error with empty writeConcern object.
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{_id: 1}, {_id: 1}],
    writeConcern: {},
    ordered: false
};
result = coll.runCommand(request);
assert(result.ok);
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert.eq(null, result.writeConcernError);
assert.eq(1, coll.find().itcount());

//
// Write error with unspecified w.
coll.remove({});
request = {
    insert: coll.getName(),
    documents: [{_id: 1}, {_id: 1}],
    writeConcern: {wtimeout: 1},
    ordered: false
};
result = assert.commandWorkedIgnoringWriteErrors(coll.runCommand(request));
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert.eq(null, result.writeConcernError);
assert.eq(1, coll.find().itcount());

jsTest.log("DONE no journal/repl tests");
rst.stopSet();
})();
