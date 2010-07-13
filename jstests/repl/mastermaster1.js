// basic testing of master/master


ports = allocatePorts( 2 )

left = startMongodTest( ports[0] , "mastermaster1left" , false , { master : "" , slave : "" , source : "127.0.0.1:" + ports[1] } )
right = startMongodTest( ports[1] , "mastermaster1left" , false , { master : "" , slave : "" , source : "127.0.0.1:" + ports[0] } )

x = left.getDB( "admin" ).runCommand( "ismaster" )
assert( x.ismaster , "left: " + tojson( x ) )

x = right.getDB( "admin" ).runCommand( "ismaster" )
assert( x.ismaster , "right: " + tojson( x ) )

ldb = left.getDB( "test" )
rdb = right.getDB( "test" )

ldb.foo.insert( { _id : 1 , x : "eliot" } )
ldb.runCommand( { getlasterror : 1 , w : 2 } )
rdb.foo.insert( { _id : 2 , x : "sara" } )
rdb.runCommand( { getlasterror : 1 , w : 2 } )

assert.eq( 2 , ldb.foo.count() , "B1" )
assert.eq( 2 , rdb.foo.count() , "B2" )



for ( var i=0; i<ports.length; i++ ){
    stopMongod( ports[i] );
}




