// basic testing of master/master


ports = allocatePorts( 2 )

left = startMongodTest( ports[0] , "mastermaster1left" , false , { master : "" , slave : "" , source : "127.0.0.1:" + ports[1] } )

x = left.getDB( "admin" ).runCommand( "ismaster" )
assert( x.ismaster , "left: " + tojson( x ) )

right = startMongodTest( ports[1] , "mastermaster1right" , false , { master : "" , slave : "" , source : "127.0.0.1:" + ports[0] } )

x = right.getDB( "admin" ).runCommand( "ismaster" )
assert( x.ismaster , "right: " + tojson( x ) )

print( "check 1" )


ldb = left.getDB( "test" )
rdb = right.getDB( "test" )

print( "check 2" )

ldb.foo.insert( { _id : 1 , x : "eliot" } )
result = ldb.runCommand( { getlasterror : 1 , w : 2 , wtimeout : 40000 } );
printjson(result);
rdb.foo.insert( { _id : 2 , x : "sara" } )
result = rdb.runCommand( { getlasterror : 1 , w : 2 , wtimeout : 40000 } )
printjson(result);

print( "check 3" )

print( "left" )
ldb.foo.find().forEach( printjsononeline )
print( "right" )
rdb.foo.find().forEach( printjsononeline )

print( "oplog" )

rdb.getSisterDB( "local" ).getCollection( "oplog.$main" ).find().forEach( printjsononeline )

/*
assert.eq( 2 , ldb.foo.count() , "B1" )
assert.eq( 2 , rdb.foo.count() , "B2" )
*/

print( "going to stop everything" )

for ( var i=0; i<ports.length; i++ ){
    stopMongod( ports[i] );
}

print( "yay" )


