var name = "dumprestore7";

function step(msg) {
    msg = msg || "";
    this.x = (this.x || 0) + 1;
    print('\n' + name + ".js step " + this.x + ' ' + msg);
}

step();

var replTest = new ReplSetTest( {name: name, nodes: 1} );
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
    var time = replTest.getMaster().getDB("local").getCollection("oplog.rs").find().limit(1).sort({$natural:-1}).next();
    step(time.ts.t);
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

var data = "/data/db/dumprestore7-dump1/";
var query = "{\"ts\":{\"$gt\":{\"$timestamp\" : {\"t\":"+ time.ts.t + ",\"i\":" + time.ts.i +" }}}}";

runMongoProgram( "mongodump", "--host", "127.0.0.1:"+replTest.ports[0], "--db", "local", "--collection", "oplog.rs", "--query", query, "--out", data );

step("try mongorestore from $timestamp");

runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+port, "--dir", data );
var x = 9;
x = conn.getDB("local").getCollection("oplog.rs").count();

assert.eq(x, 20, "mongorestore should only have the latter 20 entries");

step("stopSet");
replTest.stopSet();

step("SUCCESS");

