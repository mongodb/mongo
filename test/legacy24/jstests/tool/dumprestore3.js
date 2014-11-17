// dumprestore3.js

var name = "dumprestore3";

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
    step("populate master");
    var foo = master.getDB("foo");
    for (i = 0; i < 20; i++) {
        foo.bar.insert({ x: i, y: "abc" });
    }
}

{
    step("wait for slaves");
    replTest.awaitReplication();
}

{
    step("dump & restore a db into a slave");
    var port = 30020;
    var conn = startMongodTest(port, name + "-other");
    var c = conn.getDB("foo").bar;
    c.save({ a: 22 });
    assert.eq(1, c.count(), "setup2");
}

step("try mongorestore to slave");

var data = "/data/db/dumprestore3-other1/";
resetDbpath(data);
runMongoProgram( "mongodump", "--host", "127.0.0.1:"+port, "--out", data );

var x = runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+replTest.ports[1], "--dir", data );
assert.eq(x, _isWindows() ? -1 : 255, "mongorestore should exit w/ -1 on slave");

step("try mongoimport to slave");

dataFile = "/data/db/dumprestore3-other2.json";
runMongoProgram( "mongoexport", "--host", "127.0.0.1:"+port, "--out", dataFile, "--db", "foo", "--collection", "bar" );

x = runMongoProgram( "mongoimport", "--host", "127.0.0.1:"+replTest.ports[1], "--file", dataFile );
assert.eq(x, _isWindows() ? -1 : 255, "mongoreimport should exit w/ -1 on slave"); // windows return is signed

step("stopSet");
replTest.stopSet();

step("SUCCESS");
