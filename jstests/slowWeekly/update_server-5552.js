

load( "jstests/libs/slow_weekly_util.js" )
testServer = new SlowWeeklyMongod( "update_server-5552" )
db = testServer.getDB( "test" );

t = db.foo;
t.drop()

N = 10000;

for ( i=0; i<N; i++ ) 
    t.insert( { _id : i , x : 1 } )
db.getLastError();

join = startParallelShell( "while( db.foo.findOne( { _id : 0 } ).x == 1 ); db.foo.ensureIndex( { x : 1 } );" )

t.update( { $where : function(){ sleep(1); return true; } } , { $set : { x : 5 } } , false , true );
db.getLastError();

join();

assert.eq( N , t.find( { x : 5 } ).count() );

testServer.stop();
