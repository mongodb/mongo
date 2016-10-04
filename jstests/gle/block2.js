/**
 * Basic gle testing for master/slave environment. Write command version also
 * available at jstests/repl.
 */

var rt = new ReplTest("block1");

var m = rt.start(true);
var s = rt.start(false);

if (m.writeMode() == 'commands') {
    jsTest.log('Skipping test since commands mode is already tested in repl/');
} else {
    function setup() {
        dbm = m.getDB("foo");
        dbs = s.getDB("foo");

        tm = dbm.bar;
        ts = dbs.bar;
    }
    setup();

    function check(msg) {
        assert.eq(tm.count(), ts.count(), "check: " + msg);
    }

    function worked(w, wtimeout) {
        var gle = dbm.getLastError(w, wtimeout);
        if (gle != null) {
            printjson(gle);
        }
        return gle == null;
    }

    check("A");

    tm.save({x: 1});
    assert(worked(2), "B");

    tm.save({x: 2});
    assert(worked(2, 3000), "C");

    rt.stop(false);
    tm.save({x: 3});
    assert.eq(3, tm.count(), "D1");
    assert(!worked(2, 3000), "D2");

    s = rt.start(false);
    setup();
    assert(worked(2, 30000), "E");
}

rt.stop();
