// simple test to ensure write concern functions as expected

var name = "dumprestore10";

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
var total = 1000;

{
    step("store data");
    var foo = master.getDB("foo");
    for (i = 0; i < total; i++) {
        foo.bar.insert({ x: i, y: "abc" });
    }
}

{
    step("wait");
    replTest.awaitReplication();
}

step("mongodump from replset");

var data = "/data/db/dumprestore10-dump1/";

runMongoProgram( "mongodump", "--host", "127.0.0.1:"+replTest.ports[0], "--out", data );


{
    step("remove data after dumping");
    master.getDB("foo").getCollection("bar").drop();
}

{
    step("wait");
    replTest.awaitReplication();
}

step("try mongorestore with write concern");

runMongoProgram( "mongorestore", "--w", "2", "--host", "127.0.0.1:"+replTest.ports[0], "--dir", data );

var x = 0;

// no waiting for replication
x = master.getDB("foo").getCollection("bar").count();

assert.eq(x, total, "mongorestore should have successfully restored the collection");

step("stopSet");
replTest.stopSet();

step("SUCCESS");
