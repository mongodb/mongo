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
var maxSecondChunk = 50;
{
    step("second chunk of data");
    var foo = master.getDB("foo");
    for (i = 30; i < maxSecondChunk; i++) {
        foo.bar.insert({ x: i, y: "abc"});
    }
}
{
    var port = 30020;
    var conn = startMongodTest(port, name + "-other");
}

step("try mongodump with $timestamp");

var data = MongoRunner.dataDir + "/dumprestore7-dump1/";
var query = "{\"ts\":{\"$gt\":{\"$timestamp\" : {\"t\":"+ time.ts.t + ",\"i\":" + time.ts.i +" }}}}";

runMongoProgram( "mongodump", "--host", "127.0.0.1:"+replTest.ports[0], "--db", "local", "--collection", "oplog.rs", "--query", query, "--out", data );

step("try mongorestore from $timestamp");

runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+port, "--dir", data );
var x = 9;
x = conn.getDB("local").getCollection("oplog.rs").count();

assert.eq(x, 20, "mongorestore should only have the latter 20 entries");

var data2 = MongoRunner.dataDir + "/dumprestore7-dump2/";
step ("try mongodump with sort, skip, limit")
var skip=5, limit=15;
runMongoProgram( "mongodump", "--host", "127.0.0.1:"+replTest.ports[0], "--db", "foo", "--collection", "bar", "--sort", "{x:-1,y:1}", "--skip", "5", "--limit", "15", "--out", data2 );

step("remove any previously loaded data");
foo.bar.drop();

step("try mongorestore from data: sort, skip, limit");
runMongoProgram( "mongorestore", "--host", "127.0.0.1:"+replTest.ports[0], "--dir", data2 );

assert.eq(maxSecondChunk-skip-1, foo.bar.findOne().x, "reverse sorted order by x");
assert.eq(limit, foo.bar.count(), "limit value loaded back");

step("stopSet");
replTest.stopSet();

step("SUCCESS");

