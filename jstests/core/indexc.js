
t = db.indexc;
t.drop();

for (var i = 1; i < 100; i++) {
    var d = new Date((new Date()).getTime() + i);
    t.save({a: i, ts: d, cats: [i, i + 1, i + 2]});
    if (i == 51)
        mid = d;
}

assert.eq(50, t.find({ts: {$lt: mid}}).itcount(), "A");
assert.eq(50, t.find({ts: {$lt: mid}}).sort({ts: 1}).itcount(), "B");

t.ensureIndex({ts: 1, cats: 1});
t.ensureIndex({cats: 1});

// multi-key bug was firing here (related to getsetdup()):
assert.eq(50, t.find({ts: {$lt: mid}}).itcount(), "C");
assert.eq(50, t.find({ts: {$lt: mid}}).sort({ts: 1}).itcount(), "D");
