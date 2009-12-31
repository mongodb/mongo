
t = db.arrayfind1;
t.drop();

t.save( { a : [ { x : 1 } ] } )
t.save( { a : [ { x : 1 , y : 2 , z : 1 } ] } )
t.save( { a : [ { x : 1 , y : 1 , z : 3 } ] } )

function test( exptected , q , name ){
    assert.eq( exptected , t.find( q ).itcount() , name + " " + tojson( q ) + " itcount" );
    assert.eq( exptected , t.find( q ).count() , name + " " + tojson( q ) + " count" );
}

test( 3 , {} , "A" );
test( 1 , { "a.y" : 2 } , "B" );
test( 1 , { "a" : { x : 1 } } , "C" ); 
test( 3 , { "a" : { $elemMatch : { x : 1 } } } , "D" ); // SERVER-377


t.ensureIndex( { "a.x" : 1 } );
assert( t.find( { "a" : { $elemMatch : { x : 1 } } } ).explain().cursor.indexOf( "BtreeC" ) == 0 );

