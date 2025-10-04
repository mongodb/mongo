// @tags: [
//   requires_getmore,
// ]

// Unsorted plan on {a:1}, sorted plan on {b:1}.  The unsorted plan exhausts its memory limit before
// the sorted plan is chosen by the query optimizer.

let t = db.jstests_sortf;
t.drop();

t.createIndex({a: 1});
t.createIndex({b: 1});

for (let i = 0; i < 100; ++i) {
    t.save({a: 0, b: 0});
}

let big = new Array(10 * 1000 * 1000).toString();
for (let i = 0; i < 5; ++i) {
    t.save({a: 1, b: 1, big: big});
}

assert.eq(5, t.find({a: 1}).sort({b: 1}).itcount());
t.drop();
