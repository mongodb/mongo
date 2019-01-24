// Test aggregating a sharded collection while using $graphLookup on an unsharded collection.
(function() {
    'use strict';

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: "test.foo", key: {_id: "hashed"}}));

    let db = st.s0.getDB("test");

    assert.writeOK(db.foo.insert([{}, {}, {}, {}]));
    assert.writeOK(db.bar.insert({_id: 1, x: 1}));

    const res = db.foo
                    .aggregate([{
                        $graphLookup: {
                            from: "bar",
                            startWith: {$literal: 1},
                            connectFromField: "x",
                            connectToField: "_id",
                            as: "res"
                        }
                    }])
                    .toArray();

    assert.eq(res.length, 4);
    res.forEach(function(c) {
        assert.eq(c.res.length, 1);
        assert.eq(c.res[0]._id, 1);
        assert.eq(c.res[0].x, 1);
    });

    st.stop();
})();
