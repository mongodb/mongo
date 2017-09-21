// Test persistence of list of dbs to add.

var getDBNamesNoThrow = function(conn) {
    try {
        return conn.getDBNames();
    } catch (e) {
        printjson(e);
        return [""];
    }
};

doTest = function(signal, extraOpts) {

    rt = new ReplTest("repl7tests");

    m = rt.start(true);

    // Use databases that lexicographically follow "admin" to avoid failing to clone admin, since
    // as of SERVER-29452 mongod fails to start up without a featureCompatibilityVersion document.
    for (n = "b"; n != "bbbbb"; n += "b") {
        m.getDB(n).b.save({x: 1});
    }

    s = rt.start(false, extraOpts);

    assert.soon(function() {
        return -1 != getDBNamesNoThrow(s).indexOf("bb");
    }, "bb timeout", 60000, 1000);

    rt.stop(false, signal);

    s = rt.start(false, extraOpts, signal);

    assert.soon(function() {
        for (n = "b"; n != "bbbbb"; n += "b") {
            if (-1 == getDBNamesNoThrow(s).indexOf(n))
                return false;
        }
        return true;
    }, "b-bbbb timeout", 60000, 1000);

    assert.soon(function() {
        for (n = "b"; n != "bbbbb"; n += "b") {
            if (1 != m.getDB(n).b.find().count()) {
                return false;
            }
        }
        return true;
    }, "b-bbbb count timeout");

    sleep(300);

    rt.stop();
};

doTest(15);  // SIGTERM
