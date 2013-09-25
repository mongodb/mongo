t = db.jstests_in3;

t.drop();
t.ensureIndex( {i:1} );
/* NEW QUERY EXPLAIN
assert.eq( {i:[[3,3]]}, t.find( {i:{$in:[3]}} ).explain().indexBounds , "A1" );
*/
/* NEW QUERY EXPLAIN
assert.eq( {i:[[3,3],[6,6]]}, t.find( {i:{$in:[3,6]}} ).explain().indexBounds , "A2" );
*/

for ( var i=0; i<20; i++ )
    t.insert( { i : i } );

/* NEW QUERY EXPLAIN
assert.eq( 3 , t.find( {i:{$in:[3,6]}} ).explain().nscanned , "B1" )
*/
