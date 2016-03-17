// Test autoresync

var baseName = "jstests_repl3test";

soonCount = function(count) {
    assert.soon(function() {
        //                print( "check count" );
        //                print( "count: " + s.getDB( baseName ).z.find().count() + ", expected: " +
        //                count );
        return s.getDB(baseName).a.find().itcount() == count;
    });
};

doTest = function(signal) {

    print("repl3.js doTest(" + signal + ")");

    rt = new ReplTest("repl3tests");

    m = rt.start(true);
    s = rt.start(false);

    am = m.getDB(baseName).a;

    am.save({_id: new ObjectId()});
    soonCount(1);
    rt.stop(false, signal);

    big = new Array(2000).toString();
    for (i = 0; i < 1000; ++i)
        am.save({_id: new ObjectId(), i: i, b: big});

    s = rt.start(false, {autoresync: null}, true);

    // after SyncException, mongod waits 10 secs.
    sleep(15000);

    // Need the 2 additional seconds timeout, since commands don't work on an 'allDead' node.
    soonCount(1001);
    as = s.getDB(baseName).a;
    assert.eq(1, as.find({i: 0}).count());
    assert.eq(1, as.find({i: 999}).count());

    assert.commandFailed(s.getDB("admin").runCommand({"resync": 1}));

    rt.stop();
};

doTest(15);  // SIGTERM
doTest(9);   // SIGKILL

print("repl3.js OK");
