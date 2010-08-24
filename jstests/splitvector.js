// -------------------------
//  SPLITVECTOR TEST UTILS
// -------------------------

// -------------------------
// assertChunkSizes verifies that a given 'splitVec' divides the 'test.jstest_splitvector'
// collection in 'maxChunkSize' approximately-sized chunks. Its asserts fail otherwise.
// @param splitVec: an array with keys for field 'x'
//        e.g. [ { x : 1927 }, { x : 3855 }, ...
// @param numDocs: domain of 'x' field 
//        e.g. 20000
// @param maxChunkSize is in MBs.
//
assertChunkSizes = function ( splitVec , numDocs , maxChunkSize ){
    splitVec = [{ x: -1 }].concat( splitVec );
    splitVec.push( { x: numDocs+1 } );
    for ( i=0; i<splitVec.length-1; i++) { 
        min = splitVec[i];
        max = splitVec[i+1];
        size = db.runCommand( { datasize: "test.jstests_splitvector" , min: min , max: max } ).size;
        
        // It is okay for the last chunk to be  smaller. A collection's size does not
        // need to be exactly a multiple of maxChunkSize.
        if ( i < splitVec.length - 2 )
            assert.close( maxChunkSize , size , "A"+i ,  -3 );
        else
            assert.gt( maxChunkSize, size, "A"+i );
    }
}


// -------------------------
//  TESTS START HERE
// -------------------------

f = db.jstests_splitvector;
f.drop();

// -------------------------
// Case: missing paramters 

assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" } ).ok );
assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" , maxChunkSize: 1} ).ok );


// -------------------------
// Case: missing index

assert.eq( false, db.runCommand( { splitVector: "test.jstests_splitvector" , keyPattern: {x:1} , maxChunkSize: 1 } ).ok );


// -------------------------
// Case: empty collection

f.ensureIndex( { x: 1} );
assert.eq( [], db.runCommand( { splitVector: "test.jstests_splitvector" , keyPattern: {x:1} , maxChunkSize: 1 } ).splitKeys );


// -------------------------
// Case: uniform collection

f.drop();
f.ensureIndex( { x: 1 } );

// Get baseline document size
filler = "";
while( filler.length < 500 ) filler += "a";
f.save( { x: 0, y: filler } );
docSize = db.runCommand( { datasize: "test.jstests_splitvector" } ).size;
assert.gt( docSize, 500 );

// Fill collection and get split vector for 1MB maxChunkSize
numDocs = 20000;
for( i=1; i<numDocs; i++ ){
    f.save( { x: i, y: filler } );
}
res = db.runCommand( { splitVector: "test.jstests_splitvector" , keyPattern: {x:1} , maxChunkSize: 1 } );

// splitVector aims at getting half-full chunks after split
factor = 0.5; 

assert.eq( true , res.ok );
assert.close( numDocs*docSize / ((1<<20) * factor), res.splitKeys.length , "num split keys" , -1 );
assertChunkSizes( res.splitKeys , numDocs, (1<<20) * factor );
