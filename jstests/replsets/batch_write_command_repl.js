//
// Tests replication-related batch write protocol functionality
// NOTE: Basic write functionality is tested via the jstests, this file should contain
// *only* repl-specific tests.
//

var rst = new ReplSetTest({name: 'testSet', nodes: 3});
rst.startSet();
rst.initiate();

var rstConn = new Mongo(rst.getURL());
var coll = rstConn.getCollection("test.batch_write_command_repl");

jsTest.log("Starting repl tests...");

function resultOK( result ) {
    return result.ok &&
           !( 'code' in result ) &&
           !( 'errmsg' in result ) &&
           !( 'errInfo' in result ) &&
           !( 'writeErrors' in result );
};

function resultNOK( result ) {
    return !result.ok &&
           typeof( result.code ) == 'number' &&
           typeof( result.errmsg ) == 'string';
}

var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
//
// Insert tests

//
// Basic replica set insert, W:2
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w:2}});
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Multi replica set insert, W:3
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1},{a:1}],
                      writeConcern: {w:3}});
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(2, result.n);
assert.eq(2, coll.count());

//
// Basic replica set insert, W:4 timeout error
coll.remove({});
printjson( request = {insert : coll.getName(),
                   documents: [{a:1}],
                   writeConcern: {w:4, wtimeout:2*1000}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert(result.writeConcernError != null);
assert(result.writeConcernError.errInfo.wtimeout);
assert.eq(1, coll.count());

//
// TODO: More tests to come
//

jsTest.log("DONE!");
rst.stopSet();

