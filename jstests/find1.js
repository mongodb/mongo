t = db.find1;
t.drop();

lookAtDocumentMetrics = false;

if ( db.serverStatus().metrics ) {
    var ss = db.serverStatus();
    lookAtDocumentMetrics = ss.metrics.document != null && ss.metrics.queryExecutor.scanned != null;
}

print( "lookAtDocumentMetrics: " + lookAtDocumentMetrics );

if ( lookAtDocumentMetrics ) {
    // ignore mongos
    nscannedStart = db.serverStatus().metrics.queryExecutor.scanned
}


t.save( { a : 1 , b : "hi" } );
t.save( { a : 2 , b : "hi" } );

/* very basic test of $snapshot just that we get some result */
// we are assumign here that snapshot uses the id index; maybe one day it doesn't if so this would need to change then
assert( t.find({$query:{},$snapshot:1})[0].a == 1 , "$snapshot simple test 1" );
var q = t.findOne();
q.c = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";
t.save(q); // will move a:1 object to after a:2 in the file 
assert( t.find({$query:{},$snapshot:1})[0].a == 1 , "$snapshot simple test 2" );

assert( t.findOne( { a : 1 } ).b != null , "A" );
assert( t.findOne( { a : 1 } , { a : 1 } ).b == null , "B");

assert( t.find( { a : 1 } )[0].b != null , "C" );
assert( t.find( { a : 1 } , { a : 1 } )[0].b == null , "D" );
assert( t.find( { a : 1 } , { a : 1 } ).sort( { a : 1 } )[0].b == null , "D" );

id = t.findOne()._id;

assert( t.findOne( id ) , "E" );
assert( t.findOne( id ).a , "F" );
assert( t.findOne( id ).b , "G" );

assert( t.findOne( id , { a : 1 } ).a , "H" );
assert( ! t.findOne( id , { a : 1 } ).b , "I" );

assert(t.validate().valid,"not valid");

if ( lookAtDocumentMetrics ) {
    // ignore mongos
    nscannedEnd = db.serverStatus().metrics.queryExecutor.scanned
    assert.lte( nscannedStart + 16, nscannedEnd );
}
