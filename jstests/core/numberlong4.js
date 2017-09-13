// Test handling of comparison between long longs and their double approximations in btrees -
// SERVER-3719.

t = db.jstests_numberlong4;
t.drop();

if (0) {  // SERVER-3719

    t.ensureIndex({x: 1});

    Random.setRandomSeed();

    s = "11235399833116571";
    for (i = 0; i < 10000; ++i) {
        n = NumberLong(s + Random.randInt(10));
        t.insert({x: (Random.randInt(2) ? n : n.floatApprox)});
    }

    // If this does not return, there is a problem with index structure.
    t.find().hint({x: 1}).itcount();
}
