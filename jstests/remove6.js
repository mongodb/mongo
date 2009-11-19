
t = db.remove6;
t.drop();

function pop(){
    t.drop();
    for ( var i=0; i<1000; i++ ){
        t.save( { x : 1 , tags : [ "a" , "b" , "c" ] } );
    }
}

function del(){
    t.remove( { tags : { $in : [ "a" , "c" ] } } );
}

function test( n , idx ){
    pop();
    assert.eq( 1000 , t.count() , n + " A " + idx );
    if ( idx )
        t.ensureIndex( idx );
    del();
    var e = db.getLastError();
    assert( e == null , "error deleting: " + e );
    assert.eq( 0 , t.count() , n + " B " + idx );
}

test( "a" );
test( "b" , { x : 1 } );
test( "c" , { tags : 1 } );
