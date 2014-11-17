//dumpfilename1.js

//Test designed to make sure error that dumping a collection with "/" in the name doesn't crash the system.
//An error is logged and given to the user, but the other collections should dump and restore OK.  

t = new ToolTest( "dumpfilename1" );

t.startDB( "foo" );
c = t.db;
c.getCollection("df/").insert({a:3});
c.getCollection("df").insert({a:2});
t.db.getLastError(); // Ensure data is written before dumping it through a spawned process.

t.runTool( "dump" , "--out" , t.ext );

assert(c.getCollection("df/").drop(),"cannot drop 1");
assert(c.getCollection("df").drop(), "cannot drop 2");

t.runTool( "restore" , "--dir" , t.ext );

assert.eq( 0 , c.getCollection("df/").count() , "collection 1 does not restore properly" );
assert.eq( 1 , c.getCollection("df").count() , "collection 2 does not restore properly" );

t.stop();

