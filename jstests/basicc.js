// test writing to two db's at the same time.

t1 = db.jstests_basicc;
var db = db.getSisterDB("test1");
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
t1.drop();
t2.drop();
db = db.getSisterDB("test");  // put things back the way we found it