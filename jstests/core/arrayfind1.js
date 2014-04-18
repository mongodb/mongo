
t = db.arrayfind1;
t.drop();

t.save( { a : [ { x : 1 } ] } )
t.save( { a : [ { x : 1 , y : 2 , z : 1 } ] } )
t.save( { a : [ { x : 1 , y : 1 , z : 3 } ] } )

function test( exptected , q , name ){
    assert.eq( exptected , t.find( q ).itcount() , name + " " + tojson( q ) + " itcount" );
    assert.eq( exptected , t.find( q ).count() , name + " " + tojson( q ) + " count" );
}

test( 3 , {} , "A1" );
test( 1 , { "a.y" : 2 } , "A2" );
test( 1 , { "a" : { x : 1 } } , "A3" ); 
test( 3 , { "a" : { $elemMatch : { x : 1 } } } , "A4" ); // SERVER-377


t.save( { a : [ { x : 2 } ] } )
t.save( { a : [ { x : 3 } ] } )
t.save( { a : [ { x : 4 } ] } )

assert.eq( 1 , t.find( { a : { $elemMatch : { x : 2 } } } ).count() , "B1" );
assert.eq( 2 , t.find( { a : { $elemMatch : { x : { $gt : 2 } } } } ).count() , "B2" );

t.ensureIndex( { "a.x" : 1 } );
assert( t.find( { "a" : { $elemMatch : { x : 1 } } } ).explain().cursor.indexOf( "BtreeC" ) == 0 , "C1" );

assert.eq( 1 , t.find( { a : { $elemMatch : { x : 2 } } } ).count() , "D1" );

t.find( { "a.x" : 1 } ).count();
t.find( { "a.x" : { $gt : 1 } } ).count();

res = t.find( { "a" : { $elemMatch : { x : { $gt : 2 } } } } ).explain()
assert( res.cursor.indexOf( "BtreeC" ) == 0 , "D2" );
assert.eq( 2 , t.find( { a : { $elemMatch : { x : { $gt : 2 } } } } ).count() , "D3" );

assert.eq( 2 , t.find( { a : { $ne:2, $elemMatch : { x : { $gt : 2 } } } } ).count() , "E1" );
assert( t.find( { a : { $ne:2, $elemMatch : { x : { $gt : 2 } } } } ).explain().cursor.indexOf( "BtreeC" ) == 0 , "E2" );
