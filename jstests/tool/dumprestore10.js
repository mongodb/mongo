// simple test to ensure write concern functions as expected

var name = "dumprestore10";

function step(msg) {
    msg = msg || "";
    this.x = (this.x || 0) + 1;
    print('\n' + name + ".js step " + this.x + ' ' + msg);
}

step();

var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();
var total = 1000;

{
    step("store data");
    var foo = master.getDB("foo");
    for (i = 0; i < total; i++) {
        foo.bar.insert({x: i, y: "abc"});
    }
}

{
    step("wait");
    replTest.awaitReplication();
}

step("bongodump from replset");

var data = BongoRunner.dataDir + "/dumprestore10-dump1/";

var exitCode = BongoRunner.runBongoTool("bongodump", {
    host: "127.0.0.1:" + master.port,
    out: data,
});
assert.eq(0, exitCode, "bongodump failed to dump data from the replica set");

{
    step("remove data after dumping");
    master.getDB("foo").getCollection("bar").drop();
}

{
    step("wait");
    replTest.awaitReplication();
}

step("try bongorestore with write concern");

exitCode = BongoRunner.runBongoTool("bongorestore", {
    writeConcern: "2",
    host: "127.0.0.1:" + master.port,
    dir: data,
});
assert.eq(
    0, exitCode, "bongorestore failed to restore the data to a replica set while using w=2 writes");

var x = 0;

// no waiting for replication
x = master.getDB("foo").getCollection("bar").count();

assert.eq(x, total, "bongorestore should have successfully restored the collection");

step("stopSet");
replTest.stopSet();

step("SUCCESS");
