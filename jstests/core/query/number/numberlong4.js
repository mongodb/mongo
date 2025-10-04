// Test handling of comparison between long longs and their double approximations in btrees -
// SERVER-3719.
// @tags: [
//   requires_getmore,
// ]

let t = db.jstests_numberlong4;
t.drop();

if (0) {
    // SERVER-3719

    t.createIndex({x: 1});

    Random.setRandomSeed();

    let s = "11235399833116571";
    for (let i = 0; i < 10000; ++i) {
        let n = NumberLong(s + Random.randInt(10));
        t.insert({x: Random.randInt(2) ? n : n.floatApprox});
    }

    // If this does not return, there is a problem with index structure.
    t.find().hint({x: 1}).itcount();
}
