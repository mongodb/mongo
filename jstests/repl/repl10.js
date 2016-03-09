// Test slave delay

var baseName = "jstests_repl10test";

soonCount = function(count) {
    assert.soon(function() {
        //                print( "check count" );
        //                print( "count: " + s.getDB( baseName ).z.find().count() );
        return s.getDB(baseName).a.find().count() == count;
    });
};

doTest = function(signal) {

    rt = new ReplTest("repl10tests");

    m = rt.start(true);
    s = rt.start(false, {"slavedelay": "10"});

    am = m.getDB(baseName).a;

    am.save({i: 1});

    soonCount(1);

    am.save({i: 2});
    assert.eq(2, am.count());
    sleep(3000);
    assert.eq(1, s.getDB(baseName).a.count());

    soonCount(2);

    rt.stop();
};

print("repl10.js dotest(15)");
doTest(15);  // SIGTERM
print("repl10.js dotest(15)");
doTest(9);  // SIGKILL
print("repl10.js SUCCESS");
