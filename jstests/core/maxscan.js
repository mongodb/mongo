(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

    const coll = db.maxscan;
    coll.drop();

    const N = 100;
    for (let i = 0; i < N; i++) {
        assert.writeOK(coll.insert({_id: i, x: i % 10}));
    }

    assert.eq(N, coll.find().itcount());
    // We should scan no more than 50 things on each shard.
    assert.lte(coll.find().maxScan(50).itcount(),
               50 * FixtureHelpers.numberOfShardsForCollection(coll));

    assert.eq(coll.find({x: 2}).itcount(), 10);
    // We should scan no more than 50 things on each shard.
    assert.lte(coll.find({x: 2}).sort({_id: 1}).maxScan(50).itcount(),
               5 * FixtureHelpers.numberOfShardsForCollection(coll));

    assert.commandWorked(coll.ensureIndex({x: 1}));
    assert.eq(coll.find({x: 2}).sort({_id: 1}).hint({x: 1}).maxScan(N).itcount(), 10);
    assert.eq(coll.find({x: 2}).sort({_id: 1}).hint({x: 1}).maxScan(1).itcount(), 0);
}());
