//
// Tests write-concern-related batch write protocol functionality
//

var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

jsTest.log("Starting no journal/repl set tests...");

// Start a single-node replica set with no journal
// Allows testing immediate write concern failures and wc application failures
var rst = new ReplSetTest({ nodes : 2 });
rst.startSet({ nojournal : "" });
rst.initiate();
var mongod = rst.getPrimary();
var coll = mongod.getCollection("test.batch_write_command_wc");

//
// Basic insert, default WC
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}]});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Basic insert, majority WC
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w: 'majority'}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Basic insert,  w:2 WC
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w:2}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Basic insert, immediate nojournal error
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {j:true}});
printjson( result = coll.runCommand(request) );
assert(!result.ok);
assert.eq(0, coll.count());

//
// Basic insert, timeout wc error
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w:3, wtimeout: 1}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError);
assert(result.writeConcernError.errInfo.wtimeout);
assert.eq(1, coll.count());

//
// Basic insert, wmode wc error
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w: 'invalid'}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError);
assert.eq(1, coll.count());

//
// Two ordered inserts, write error and wc error not reported
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1},{$invalid:'doc'}],
                      writeConcern: {w: 'invalid'}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert(!result.writeConcernError);
assert.eq(1, coll.count());

//
// Two unordered inserts, write error and wc error reported
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1},{$invalid:'doc'}],
                      writeConcern: {w: 'invalid'},
                      ordered: false});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(result.writeErrors.length, 1);
assert.eq(result.writeErrors[0].index, 1);
assert(result.writeConcernError);
assert.eq(1, coll.count());

jsTest.log("DONE no journal/repl tests");
rst.stopSet();

