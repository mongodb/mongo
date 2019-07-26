// Tests that the sort specification is obeyed when the query contains $near/$nearSphere.
(function() {
'use strict';

const st = new ShardingTest({shards: 2});
const db = st.getDB("test");
const coll = db.geo_near_sort;
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};

assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split the data into 2 chunks and move the chunk with _id > 0 to shard 1.
assert.commandWorked(st.s0.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    st.s0.adminCommand({movechunk: coll.getFullName(), find: {_id: 1}, to: st.shard1.shardName}));

// Insert some documents. The sort order by distance from the origin is [-2, 1, -1, 2] (under 2d
// or 2dsphere geometry). The sort order by {a: 1} under the case-insensitive collation is [2,
// -1, 1, -2]. The sort order by {b: 1} is [2. -1, 1, -2].
const docMinus2 = {
    _id: -2,
    geo: [0, 0],
    a: "BB",
    b: 3
};
const docMinus1 = {
    _id: -1,
    geo: [0, 2],
    a: "aB",
    b: 1
};
const doc1 = {
    _id: 1,
    geo: [0, 1],
    a: "Ba",
    b: 2
};
const doc2 = {
    _id: 2,
    geo: [0, 3],
    a: "aa",
    b: 0
};
assert.writeOK(coll.insert(docMinus2));
assert.writeOK(coll.insert(docMinus1));
assert.writeOK(coll.insert(doc1));
assert.writeOK(coll.insert(doc2));

function testSortOrders(query, indexSpec) {
    assert.commandWorked(coll.createIndex(indexSpec));

    // Test a $near/$nearSphere query without a specified sort. The results should be sorted by
    // distance from the origin.
    let res = coll.find(query).toArray();
    assert.eq(res.length, 4, tojson(res));
    assert.eq(res[0], docMinus2, tojson(res));
    assert.eq(res[1], doc1, tojson(res));
    assert.eq(res[2], docMinus1, tojson(res));
    assert.eq(res[3], doc2, tojson(res));

    // Test with a limit.
    res = coll.find(query).limit(2).toArray();
    assert.eq(res.length, 2, tojson(res));
    assert.eq(res[0], docMinus2, tojson(res));
    assert.eq(res[1], doc1, tojson(res));

    if (db.getMongo().useReadCommands()) {
        // Test a $near/$nearSphere query sorted by {a: 1} with the case-insensitive collation.
        res = coll.find(query).collation(caseInsensitive).sort({a: 1}).toArray();
        assert.eq(res.length, 4, tojson(res));
        assert.eq(res[0], doc2, tojson(res));
        assert.eq(res[1], docMinus1, tojson(res));
        assert.eq(res[2], doc1, tojson(res));
        assert.eq(res[3], docMinus2, tojson(res));

        // Test with a limit.
        res = coll.find(query).collation(caseInsensitive).sort({a: 1}).limit(2).toArray();
        assert.eq(res.length, 2, tojson(res));
        assert.eq(res[0], doc2, tojson(res));
        assert.eq(res[1], docMinus1, tojson(res));
    }

    // Test a $near/$nearSphere query sorted by {b: 1}.
    res = coll.find(query).sort({b: 1}).toArray();
    assert.eq(res.length, 4, tojson(res));
    assert.eq(res[0], doc2, tojson(res));
    assert.eq(res[1], docMinus1, tojson(res));
    assert.eq(res[2], doc1, tojson(res));
    assert.eq(res[3], docMinus2, tojson(res));

    // Test with a limit.
    res = coll.find(query).sort({b: 1}).limit(2).toArray();
    assert.eq(res.length, 2, tojson(res));
    assert.eq(res[0], doc2, tojson(res));
    assert.eq(res[1], docMinus1, tojson(res));

    assert.commandWorked(coll.dropIndex(indexSpec));
}

testSortOrders({geo: {$near: [0, 0]}}, {geo: "2d"});
testSortOrders({geo: {$nearSphere: [0, 0]}}, {geo: "2d"});
testSortOrders({geo: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}}},
               {geo: "2dsphere"});
testSortOrders({geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}},
               {geo: "2dsphere"});

st.stop();
})();
