//
// Ensures that mongod respects the batch write protocol for inserts
//

var coll = db.getCollection( "batch_write_insert" );
coll.drop();

assert(coll.getDB().getMongo().useWriteCommands(), "test is not running with write commands")

jsTest.log("Starting insert tests...");

var request;
var result;
var batch;

var maxWriteBatchSize = 1000;

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
// Large batch under the size threshold should insert successfully
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize; ++i) {
    batch.push({});
}
printjson( request = {insert : coll.getName(), documents: batch, writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(batch.length, result.n);
assert.eq(coll.count(), batch.length);

//
// Large batch above the size threshold should fail to insert
coll.remove({});
batch = [];
for (var i = 0; i < maxWriteBatchSize + 1; ++i) {
    batch.push({});
}
printjson( request = {insert : coll.getName(), documents: batch, writeConcern:{w:1}, ordered:false} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(coll.count(), 0);

//
// Batch of size zero should fail to insert
coll.remove({});
printjson( request = {insert : coll.getName(),
                      documents: [] } );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));

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

//
// Ensure _id is the first field in all documents
coll.remove({});
printjson(request = {insert : coll.getName(), documents : [{a : 1}, {a : 2, _id : 2}]});
printjson(result = coll.runCommand(request));
assert.eq(2, coll.count());
coll.find().forEach(function(doc) {
    var firstKey = null;
    for ( var key in doc) {
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
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(), key : {x : 1}, name : "x_1"}]});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(coll.getIndexes().length, 2);

//
// Background index creation
coll.drop();
coll.insert({ x : 1 });
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(),
                                   key : {x : 1},
                                   name : "x_1",
                                   background : true}]});
printjson( result = coll.runCommand(request) );
assert(result.ok);
assert.eq(1, result.n);
assert.eq(coll.getIndexes().length, 2);

//
// Duplicate index insertion gives n = 0
coll.drop();
coll.ensureIndex({x : 1}, {unique : true});
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(),
                                   key : {x : 1}, name : "x_1", unique : true}]});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(0, result.n);
assert(!('writeErrors' in result));
assert.eq(coll.getIndexes().length, 2);

//
// Invalid index insertion with mismatched collection db
coll.drop();
printjson(request = {insert : "system.indexes",
                     documents : [{ns : "invalid." + coll.getName(),
                                   key : {x : 1}, name : "x_1", unique : true}]});
printjson(result = coll.runCommand(request));
assert(!result.ok);
assert.eq(coll.getIndexes().length, 0);

//
// Empty index insertion
coll.drop();
printjson(request = {insert : "system.indexes", documents : [{}]});
printjson(result = coll.runCommand(request));
assert(!result.ok);
assert.eq(coll.getIndexes().length, 0);

//
// Invalid index desc
coll.drop();
printjson(request = {insert : "system.indexes", documents : [{ns : coll.toString()}]});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 1);

//
// Invalid index desc
coll.drop();
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(), key : {x : 1}}]});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 1);

//
// Invalid index desc
coll.drop();
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(), name : "x_1"}]});
printjson(result = coll.runCommand(request));
assert(result.ok);
assert.eq(0, result.n);
assert.eq(0, result.writeErrors[0].index);
assert.eq(coll.getIndexes().length, 1);

//
// Cannot insert more than one index at a time through the batch writes
coll.drop();
printjson(request = {insert : "system.indexes",
                     documents : [{ns : coll.toString(), key : {x : 1}, name : "x_1"},
                                  {ns : coll.toString(), key : {y : 1}, name : "y_1"}]});
printjson(result = coll.runCommand(request));
assert(!result.ok);
assert.eq(coll.getIndexes().length, 0);

