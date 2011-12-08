//dumpfilename1.js

//Test designed to make sure proper error is thrown when trying to dump collection with a "/" in the name

t = new ToolTest( "dumpfilename1" );

t.startDB( "foo" );

db = t.db;
db.getCollection("df1/").insert({a:3});
db.getCollection("df1").insert({a:2});
t.runTool( "dump" , "--out" , t.ext );

catch(e){
    print(e.message);
    assert(false,"fail here");
    t.stop();
}
assert(false,"fail at end");
t.stop();