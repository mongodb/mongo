(function() {
    "use strict";

    const coll = db.maxscan;
    coll.drop();

    const N = 100;
    for (let i = 0; i < N; i++) {
        assert.writeOK(coll.insert({_id: i, x: i % 10}));
    }

    assert.eq(N, coll.find().itcount(), "A");
    assert.eq(50, coll.find().maxScan(50).itcount(), "B");

    assert.eq(10, coll.find({x: 2}).itcount(), "C");
    assert.eq(5, coll.find({x: 2}).sort({_id: 1}).maxScan(50).itcount(), "D");

    assert.commandWorked(coll.ensureIndex({x: 1}));
    assert.eq(10, coll.find({x: 2}).sort({_id: 1}).hint({x: 1}).maxScan(N).itcount(), "E");
    assert.eq(0, coll.find({x: 2}).sort({_id: 1}).hint({x: 1}).maxScan(1).itcount(), "E");
}());
