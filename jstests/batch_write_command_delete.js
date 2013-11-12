//
// Ensures that mongod respects the batch write protocols for delete
//

var coll = db.getCollection( "batch_write_delete" );

jsTest.log("Starting delete tests...");

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
coll.insert({a:1});
printjson( request = {'delete' : coll.getName()} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, coll.count());

//
// Single document remove, default write concern specified
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a:1}, limit: 1}]} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(0, coll.count());

//
// Single document delete, w:0 write concern specified
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a: 1}, limit: 1}],
                      writeConcern:{w:0}} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(0, coll.count());

//
// Single document remove, w:1 write concern specified, ordered:true
coll.remove({});
coll.insert([{a:1}, {a:1}]);
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a: 1}, limit: 1}],
                      writeConcern:{w:1},
                      ordered: false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(1, coll.count());

//
// Multiple document remove, w:1 write concern specified, ordered:true, default top
coll.remove({});
coll.insert([{a:1}, {a:1}]);
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a: 1}, limit: 0}],
                      writeConcern:{w:1},
                      ordered: false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(2, result.n);
assert.eq(0, coll.count());

//
// Multiple document remove, w:1 write concern specified, ordered:true, top:0
coll.remove({});
coll.insert([{a:1}, {a:1}]);
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a: 1}, limit: 0}],
                      writeConcern:{w:1},
                      ordered: false} );
printjson( result = coll.runCommand(request) );
assert(resultOK(result));
assert.eq(2, result.n);
assert.eq(0, coll.count());

//
// Cause remove error using ordered:true
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {a:1}, limit: 0},
                                {q: {$set: {a: 1}}, limit: 0},
                                {q: {$set: {a: 1}}, limit: 0}],
                      writeConcern:{w:1},
                      ordered: true} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, result.n);
assert.eq(1, result.errDetails.length);

assert.eq(1, result.errDetails[0].index);
assert.eq('number', typeof result.errDetails[0].code);
assert.eq('string', typeof result.errDetails[0].errmsg);
assert.eq(0, coll.count());

//
// Cause remove error using ordered:false
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {$set: {a: 1}}, limit: 0},
                                {q: {$set: {a: 1}}, limit: 0},
                                {q: {a:1}, limit: 0}],
                      writeConcern:{w:1},
                      ordered: false} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, result.n);
assert.eq(2, result.errDetails.length);

assert.eq(0, result.errDetails[0].index);
assert.eq('number', typeof result.errDetails[0].code);
assert.eq('string', typeof result.errDetails[0].errmsg);

assert.eq(1, result.errDetails[1].index);
assert.eq('number', typeof result.errDetails[1].code);
assert.eq('string', typeof result.errDetails[1].errmsg);
assert.eq(0, coll.count());

//
// Cause remove error using ordered:false and w:0
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {$set: {a: 1}}, limit: 0},
                                {q: {$set: {a: 1}}, limit: 0},
                                {q: {a:1}, limit: 0}],
                      writeConcern:{w:0},
                      ordered: false} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(1, result.n);
assert(!('errDetails' in result));
assert.eq(0, coll.count());

//
// Cause remove error using ordered:true and w:0
coll.remove({});
coll.insert({a:1});
printjson( request = {'delete' : coll.getName(),
                      deletes: [{q: {$set: {a: 1}}, limit: 0},
                                {q: {$set: {a: 1}}, limit: 0},
                                {q: {a:1}, limit:(1)}],
                      writeConcern:{w:0},
                      ordered: true} );
printjson( result = coll.runCommand(request) );
assert(resultNOK(result));
assert.eq(0, result.n);
assert(!('errDetails' in result));
assert.eq(1, coll.count());

