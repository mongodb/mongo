//
// Tests write-concern-related batch write protocol functionality for master/slave replication
// More general write concern tests in replsets/batch_write_command_wc.js
//

var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

jsTest.log("Starting legacy repl tests...");

// Start a master node
// Allows testing legacy repl failures
var mongod = MongoRunner.runMongod({master: "", oplogSize: 40, smallfiles: ""});
var coll = mongod.getCollection("test.batch_write_command_wc_repl");

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
assert.eq(1, coll.count());

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
assert.eq(1, coll.count());

//
// Basic insert, immediate bad wMode error
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 'invalid'}
});
printjson(result = coll.runCommand(request));
assert(!result.ok);
assert.eq(0, coll.count());

//
// Basic insert, error on WC with wtimeout
coll.remove({});
printjson(request = {
    insert: coll.getName(),
    documents: [{a: 1}],
    writeConcern: {w: 2, wtimeout: 1}
});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError);
assert(result.writeConcernError.errInfo.wtimeout);
assert.eq(1, coll.count());

jsTest.log("DONE legacy repl tests");
MongoRunner.stopMongod(mongod);
