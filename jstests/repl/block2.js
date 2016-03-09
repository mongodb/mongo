
var rt = new ReplTest("block1");

m = rt.start(true);
s = rt.start(false);

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

check("A");

assert.writeOK(tm.insert({x: 1}, {writeConcern: {w: 2}}));
assert.writeOK(tm.insert({x: 2}, {writeConcern: {w: 2, wtimeout: 3000}}));

rt.stop(false);
assert.writeError(tm.insert({x: 3}, {writeConcern: {w: 2, wtimeout: 3000}}));
assert.eq(3, tm.count(), "D1");

rt.stop();
