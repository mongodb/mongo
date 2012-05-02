// test writing to two db's at the same time.

t1 = db.jstests_basicc;
var db = db.getSisterDB("test_basicc");
t2 = db.jstests_basicc;
t1.drop();
t2.drop();

js = "while( 1 ) { db.jstests.basicc1.save( {} ); }";
pid = startMongoProgramNoConnect( "mongo" , "--eval" , js , db.getMongo().host );

for( var i = 0; i < 1000; ++i ) {
    t2.save( {} );
}
assert.automsg( "!db.getLastError()" );
stopMongoProgramByPid( pid );
// put things back the way we found it
t1.drop();
t2.drop();
db.dropDatabase();
db = db.getSisterDB("test");