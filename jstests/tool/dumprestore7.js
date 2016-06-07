var name = "dumprestore7";

function step(msg) {
    msg = msg || "";
    this.x = (this.x || 0) + 1;
    print('\n' + name + ".js step " + this.x + ' ' + msg);
}

step();

var replTest = new ReplSetTest({name: name, nodes: 1});
var nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();

{
    step("first chunk of data");
    var foo = master.getDB("foo");
    for (i = 0; i < 20; i++) {
        foo.bar.insert({x: i, y: "abc"});
    }
}

{
    step("wait");
    replTest.awaitReplication();
    var time = replTest.getPrimary()
                   .getDB("local")
                   .getCollection("oplog.rs")
                   .find()
                   .limit(1)
                   .sort({$natural: -1})
                   .next();
    step(time.ts.t);
}

{
    step("second chunk of data");
    var foo = master.getDB("foo");
    for (i = 30; i < 50; i++) {
        foo.bar.insert({x: i, y: "abc"});
    }
}
{ var conn = MongoRunner.runMongod({}); }

step("try mongodump with $timestamp");

var data = MongoRunner.dataDir + "/dumprestore7-dump1/";
var query = {ts: {$gt: {$timestamp: {t: time.ts.t, i: time.ts.i}}}};

var exitCode = MongoRunner.runMongoTool("mongodump", {
    host: "127.0.0.1:" + replTest.ports[0],
    db: "local",
    collection: "oplog.rs",
    query: tojson(query),
    out: data,
});
assert.eq(0, exitCode, "monogdump failed to dump the oplog");

step("try mongorestore from $timestamp");

exitCode = MongoRunner.runMongoTool("mongorestore", {
    host: "127.0.0.1:" + conn.port,
    dir: data,
    writeConcern: 1,
});
assert.eq(0, exitCode, "mongorestore failed to restore the oplog");

var x = 9;
x = conn.getDB("local").getCollection("oplog.rs").count();

assert.eq(x, 20, "mongorestore should only have inserted the latter 20 entries");

step("stopSet");
replTest.stopSet();

step("SUCCESS");
