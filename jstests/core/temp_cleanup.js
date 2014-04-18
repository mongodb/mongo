
mydb = db.getSisterDB( "temp_cleanup_test" )

t = mydb.tempCleanup
t.drop()

t.insert( { x : 1 } )

res = t.mapReduce( function(){ emit(1,1); } , function(){ return 1; } , "xyz" );
printjson( res );

assert.eq( 1 , t.count() , "A1" )
assert.eq( 1 , mydb[res.result].count() , "A2" )

mydb.dropDatabase()

