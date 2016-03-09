// test basic operations in parallel, with replication
load('jstests/libs/parallelTester.js');

baseName = "parallel_repl";

rt = new ReplTest(baseName);

m = rt.start(true);
s = rt.start(false);

// tests need to run against master server
db = m.getDB("test");
host = db.getMongo().host;

Random.setRandomSeed();

t = new ParallelTester();

for (id = 0; id < 10; ++id) {
    var g = new EventGenerator(id, baseName, Random.randInt(20), host);
    for (var j = 0; j < 1000; ++j) {
        var op = Random.randInt(3);
        switch (op) {
            case 0:  // insert
                g.addInsert({_id: Random.randInt(1000)});
                break;
            case 1:  // remove
                g.addRemove({_id: Random.randInt(1000)});
                break;
            case 2:  // update
                g.addUpdate({_id: {$lt: 1000}}, {$inc: {a: 5}});
                break;
            default:
                assert(false, "Invalid op code");
        }
    }
    t.add(EventGenerator.dispatch, g.getEvents());
}

var g = new EventGenerator(id, baseName, Random.randInt(5), host);
for (var j = 1000; j < 3000; ++j) {
    g.addCheckCount(j - 1000, {_id: {$gte: 1000}}, j % 100 == 0, j % 500 == 0);
    g.addInsert({_id: j});
}
t.add(EventGenerator.dispatch, g.getEvents());

t.run("one or more tests failed");

masterValidation = m.getDB("test")[baseName].validate();
assert(masterValidation.valid, tojson(masterValidation));

slaveValidation = s.getDB("test")[baseName].validate();
assert(slaveValidation.valid, tojson(slaveValidation));

assert.soon(function() {
    mh = m.getDB("test").runCommand("dbhash");
    //            printjson( mh );
    sh = s.getDB("test").runCommand("dbhash");
    //            printjson( sh );
    return mh.md5 == sh.md5;
});
