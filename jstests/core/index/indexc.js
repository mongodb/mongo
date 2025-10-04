// @tags: [
//   requires_getmore,
// ]

let t = db.indexc;
t.drop();

const startMillis = new Date().getTime();
for (let i = 1; i < 100; i++) {
    let d = new Date(startMillis + i);
    t.save({a: i, ts: d, cats: [i, i + 1, i + 2]});
    if (i == 51) var mid = d;
}

assert.eq(50, t.find({ts: {$lt: mid}}).itcount(), "A");
assert.eq(
    50,
    t
        .find({ts: {$lt: mid}})
        .sort({ts: 1})
        .itcount(),
    "B",
);

t.createIndex({ts: 1, cats: 1});
t.createIndex({cats: 1});

// multi-key bug was firing here (related to getsetdup()):
assert.eq(50, t.find({ts: {$lt: mid}}).itcount(), "C");
assert.eq(
    50,
    t
        .find({ts: {$lt: mid}})
        .sort({ts: 1})
        .itcount(),
    "D",
);
