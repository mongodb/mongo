
a = [ 1 , "asd" , null , [ 2 , 3 ] , new Date() , { x : 1 } ]

for ( var i=0; i<a.length; i++ ){
    var ret = db.eval( "function( a , i ){ return a[i]; }" , a , i );
    assert.eq( typeof( a[i] ) , typeof( ret ) , "type test" );
    assert.eq( a[i] , ret , "val test: " + typeof( a[i] ) );
}
