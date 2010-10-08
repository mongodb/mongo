// dumprestore3.js

var name = "dumprestore3";

var replTest = new ReplSetTest( {name: name, nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

// populate master
var foo = master.getDB("foo");
for (i=0; i<20; i++) {
  foo.bar.insert({x:i,y:"abc"});
}

// wait for slaves
replTest.awaitReplication();

// dump & restore a db into a slave
var port = 30020;
var conn = startMongodTest( port , name + "-other" );
var c = conn.getDB("foo").bar;
c.save( { a : 22 } );
assert.eq( 1 , c.count() , "setup2" );


// try mongorestore to slave

var data = "/data/db/dumprestore3-other1/";
resetDbpath(data);
runMongoProgram( "mongodump", "--host", "127.0.0.1:"+port, "--out", data );

var x = runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+replTest.ports[1], "--dir", data );
assert.eq(x, 255, "mongorestore should exit w/ -1 on slave");


// try mongoimport to slave

dataFile = "/data/db/dumprestore3-other2.json";
runMongoProgram( "mongoexport", "--host", "127.0.0.1:"+port, "--out", dataFile, "--db", "foo", "--collection", "bar" );

x = runMongoProgram( "mongoimport", "--host", "127.0.0.1:"+replTest.ports[1], "--file", dataFile );
assert.eq(x, 255, "mongoreimport should exit w/ -1 on slave");

replTest.stopSet();
