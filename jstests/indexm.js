// Check proper range combinations with or clauses overlapping non or portion of query SERVER-2302

t = db.jstests_indexm;
t.drop();

t.save( { a : [ { x : 1 } , { x : 2 } , { x : 3 } , { x : 4 } ] } ) 

function test(){ 
    assert.eq( 1, t.count( 
                            { 
                            a : { x : 1 } , 
                            "$or" : [ { a : { x : 2 } } , { a : { x : 3 } } ] 
                            } 
                          ) );
} 

// The first find will return a result since there isn't an index.
test(); 

// Now create an index.
t.ensureIndex({"a":1});
test();
// SERVER-3105
//assert( !t.find( 
//                { 
//                a : { x : 1 } , 
//                "$or" : [ { a : { x : 2 } } , { a : { x : 3 } } ] 
//                } 
//               ).explain().clauses );

// Now create a different index.
t.dropIndexes();
t.ensureIndex({"a.x":1});
test();

// Drop the indexes.
t.dropIndexes();
test();