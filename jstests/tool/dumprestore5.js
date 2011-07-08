// dumprestore5.js

var name = "dumprestore5";

function step(msg) {
    msg = msg || "";
    this.x = (this.x || 0) + 1;
    print('\n' + name + ".js step " + this.x + ' ' + msg);
}

step();

var replTest = new ReplSetTest( {name: name, nodes: 2} );
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();

{
    step("first chunk of data");
    var foo = master.getDB("foo");
    for (i = 0; i < 20; i++) {
        foo.bar.insert({ x: i, y: "abc" });
    }
}

{
    step("wait");
    replTest.awaitReplication();
    var time = new Date()/1 + 5000;
    sleep( 5000 );
}

{
    step("second chunk of data");
    var foo = master.getDB("foo");
    for (i = 30; i < 50; i++) {
        foo.bar.insert({ x: i, y: "abc" });
    }
}
{
    var port = 30020;
    var conn = startMongodTest(port, name + "-other");
}

step("try mongodump with $timestamp");

var data = "/data/db/dumprestore5-dump1/";
var query = "{\"ts\":{\"$gte\":{\"$timestamp\" : {"+ time + ", 0 }}}}";

runMongoProgram( "mongodump", "--host", "127.0.0.1:31001", "--db", "local", "--collection", "oplog.rs", "--query", query, "--out", data );

step("try mongorestore from $timestamp");

runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+port, "--dir", data );
var x = 9;
x = conn.getDB("local").getCollection("oplog.rs").count();

assert.eq(x, 20, "mongorestore should only have the latter 20 entries");

step("stopSet");
replTest.stopSet();

step("SUCCESS");
