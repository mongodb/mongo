load("jstests/libs/slow_weekly_util.js");
test = new SlowWeeklyMongod("conc_update");
db = test.getDB("test");
t = db.disk_reuse1;
t.drop();

N = 10000;

function k() {
    return Math.floor(Math.random() * N);
}

s = "";
while (s.length < 1024)
    s += "abc";

state = {};

var bulk = t.initializeUnorderedBulkOp();
for (var i = 0; i < N; i++) {
    bulk.insert({_id: i, s: s});
}
assert.writeOK(bulk.execute());

orig = t.stats();

t.remove({});

bulk = t.initializeUnorderedBulkOp();
for (i = 0; i < N; i++) {
    bulk.insert({_id: i, s: s});
}
assert.writeOK(bulk.execute());

assert.eq(orig.storageSize, t.stats().storageSize, "A");

for (j = 0; j < 100; j++) {
    for (i = 0; i < N; i++) {
        bulk = t.initializeUnorderedBulkOp();
        var r = Math.random();
        if (r > .5)
            bulk.find({_id: i}).remove();
        else
            bulk.find({_id: i}).upsert().updateOne({_id: i, s: s});
    }

    assert.writeOK(bulk.execute());
    assert.eq(orig.storageSize, t.stats().storageSize, "B" + j);
}

test.stop();
