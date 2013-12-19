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
           !( 'writeErrors' in result );
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
assert.eq(coll.count(), 1);

for (var field in result) {
    assert.eq('ok', field, 'unexpected field found in result: ' + field);
}

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
assert(result.ok);
assert(result.writeErrors != null);
assert.eq(1, result.writeErrors.length);
assert.eq(0, result.n);
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
print( coll.count() );
printjson( coll.findOne() );
printjson( request = {insert : coll.getName(), documents: [{a:1}]} );
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.writeErrors.length);
assert.eq(0, result.n);
assert.eq(coll.count(), 1);

//
// Fail with duplicate key error on multiple document inserts, ordered false
coll.remove({});
printjson( request = {insert : coll.getName(), documents: [{a:1}, {a:1}, {a:1}], writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(result.ok);
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
printjson( request = {insert : coll.getName(), documents: [{a:1}, {a:1}, {a:1}], writeConcern:{w:1}, ordered:true} );
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(1, result.writeErrors.length);

assert.eq(1, result.writeErrors[0].index);
assert.eq('number', typeof result.writeErrors[0].code);
assert.eq('string', typeof result.writeErrors[0].errmsg);

assert.eq(coll.count(), 1);

