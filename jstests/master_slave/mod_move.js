
// test repl basics
// data on master/slave is the same

var rt = new ReplTest("mod_move");

m = rt.start(true, {oplogSize: 50});

am = m.getDB("foo");

function check(note) {
    var start = new Date();
    var x, y;
    while ((new Date()).getTime() - start.getTime() < 5 * 60 * 1000) {
        x = am.runCommand("dbhash");
        y = as.runCommand("dbhash");
        if (x.md5 == y.md5)
            return;
        sleep(200);
    }
    assert.eq(x.md5, y.md5, note);
}

// insert a lot of 'big' docs
// so when we delete them the small docs move here

BIG = 100000;
N = BIG * 2;

var bulk = am.a.initializeUnorderedBulkOp();
for (var i = 0; i < BIG; i++) {
    bulk.insert({_id: i, s: 1, x: 1});
}
for (; i < N; i++) {
    bulk.insert({_id: i, s: 1});
}
for (i = 0; i < BIG; i++) {
    bulk.find({_id: i}).remove();
}
assert.writeOK(bulk.execute());
assert.eq(BIG, am.a.count());

if (am.serverStatus().storageEngine.name == "mmapv1") {
    assert.eq(1, am.a.stats().paddingFactor, "A2");
}

// start slave
s = rt.start(false);
as = s.getDB("foo");
bulk = am.a.initializeUnorderedBulkOp();
for (i = N - 1; i >= BIG; i--) {
    bulk.find({_id: i}).update({$set: {x: 1}});
}
assert.writeOK(bulk.execute());

check("B");

rt.stop();
