// movechunk1.js

s = new ShardingTest( "movechunk1" , 2 );

l = s._connections[0];
r = s._connections[1];

ldb = l.getDB( "foo" );
rdb = r.getDB( "foo" );

ldb.things.save( { a : 1 } )
ldb.things.save( { a : 2 } )
ldb.things.save( { a : 3 } )

assert.eq( ldb.things.count() , 3 );
assert.eq( rdb.things.count() , 0 );

startResult = l.getDB( "admin" ).runCommand( { "movechunk.start" : "foo.things" , 
                                               "to" : s._serverNames[1] , 
                                               "from" : s._serverNames[0] , 
                                               filter : { a : { $gt : 2 } }
                                             } );
print( "movechunk.start: " + tojson( startResult ) );
assert( startResult.ok == 1 , "start failed!" );

finishResult = l.getDB( "admin" ).runCommand( { "movechunk.finish" : "foo.things" , 
                                                finishToken : startResult.finishToken ,
                                                to : s._serverNames[1] , 
                                                newVersion : 1 } );
print( "movechunk.finish: " + tojson( finishResult ) );
assert( finishResult.ok == 1 , "finishResult failed!" );

assert.eq( rdb.things.count() , 1 , "right has wrong size after move" );
assert.eq( ldb.things.count() , 2 , "left has wrong size after move" );


s.stop();


