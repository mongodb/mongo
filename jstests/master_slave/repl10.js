// Test slave delay
(function() {
    "use strict";

    var baseName = "jstests_repl10test";

    var soonCount = function(s, count) {
        assert.soon(function() {
            //                print( "check count" );
            //                print( "count: " + s.getDB( baseName ).z.find().count() );
            return s.getDB(baseName).a.find().count() == count;
        });
    };

    var doTest = function(signal) {

        var rt = new ReplTest("repl10tests");

        var m = rt.start(true);
        var s = rt.start(false, {"slavedelay": "30"});

        var am = m.getDB(baseName).a;

        am.save({i: 1});

        soonCount(s, 1);

        am.save({i: 2});
        assert.eq(2, am.count());
        sleep(2000);
        assert.eq(1, s.getDB(baseName).a.count());

        soonCount(s, 2);

        rt.stop();
    };

    print("repl10.js dotest(15)");
    doTest(15);  // SIGTERM
    print("repl10.js dotest(15)");
    doTest(9);  // SIGKILL
    print("repl10.js SUCCESS");
}());