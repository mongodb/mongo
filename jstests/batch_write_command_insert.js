//
// Ensures that mongod respects the batch write protocol for inserts
//

var coll = db.getCollection( "batch_write_insert" );
coll.drop();

jsTest.log("Starting insert tests...");

var request;
var result;

function resultOK( result ) {
    return result.ok &&
           !( 'code' in result ) &&
           !( 'errmsg' in result ) &&
           !( 'errInfo' in result ) &&
           !( 'errDetails' in result );
};

function resultNOK( result ) {
    return !result.ok &&
           typeof( result.code ) == 'number' &&
           typeof( result.errmsg ) == 'string';
};

// EACH TEST BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
// NO DOCS, illegal command
coll.remove({});
printjson( request = {insert : coll.getName()} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));

//
// Single document insert, no write concern specified
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}]} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:0 write concern specified, missing ordered
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}], writeConcern:{w:0}} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:1 write concern specified, ordered:true
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}], writeConcern:{w:1}, ordered:true} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Single document insert, w:1 write concern specified, ordered:false
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}], writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
// Document with illegal key should fail
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{$set:{a:1}}], writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(coll.count(), 0);

//
// Document with valid nested key should insert (op log format)
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{o: {$set:{a:1}}}], writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(coll.count(), 1);

//
//
// Unique index tests
coll.remove({});
coll.ensureIndex({a : 1}, {unique : true});

//
// Should fail single insert due to duplicate key
coll.remove({});
coll.insert({a:1});
printjson( request = {insert : coll.getName(), documents: [{a:1}]} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered false
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}, {a:1}, {a:1}], writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, result.n);
assert.eq(2, result.errDetails.length);
assert.eq(coll.count(), 1);

assert.eq(1, result.errDetails[0].index);
assert.eq('number', typeof result.errDetails[0].code);
assert.eq('string', typeof result.errDetails[0].errmsg);

assert.eq(2, result.errDetails[1].index);
assert.eq('number', typeof result.errDetails[1].code);
assert.eq('string', typeof result.errDetails[1].errmsg);

assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered true
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}, {a:1}, {a:1}], writeConcern:{w:1}, ordered:true} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, result.n);
assert.eq(1, result.errDetails.length);

assert.eq(1, result.errDetails[0].index);
assert.eq('number', typeof result.errDetails[0].code);
assert.eq('string', typeof result.errDetails[0].errmsg);

assert.eq(coll.count(), 1);

