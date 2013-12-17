//
// Tests write-concern-related batch write protocol functionality
//

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

jsTest.log("Starting no journal/repl tests...");

var mongod = MongoRunner.runMongod({nojournal: ""});
var conn = new Mongo(mongod.host);
var coll = mongod.getCollection("test.batch_write_command_wc");

//
//
// Insert tests

//
// Basic no journal insert, default WC
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}]});
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Basic no journal insert, error on WC with j set
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1}],
                      writeConcern: {w:1, j:true}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, coll.count());
assert(result.writeConcernError != null);
assert.eq(2, result.writeConcernError.code);
assert.eq('string', typeof(result.writeConcernError.errmsg));

//
// Basic no journal insert, insert error and no write
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [{a:1, $invalid: true}],
                      writeConcern: {w:1, j:true}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(0, result.n);
assert('writeErrors' in result);
assert.eq(0, coll.count());

//
// Basic no journal insert, error on WC with j set and insert error
coll.remove({});
printjson( request = {insert : coll.getName(),
                   documents: [{a:1}, {a:1, $invalid: true}],
                   writeConcern: {w:1, j:true}});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);

assert(result.writeConcernError != null);
assert.eq(2, result.writeConcernError.code);
assert.eq('string', typeof(result.writeConcernError.errmsg));

assert.eq(1, coll.count());

//
// TODO: More tests to come
//

jsTest.log("DONE no journal/repl tests");
MongoRunner.stopMongod(mongod);

