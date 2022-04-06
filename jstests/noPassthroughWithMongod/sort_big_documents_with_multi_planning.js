// This test is designed to reproduce the test described in SERVER-65270, where a query
// which uses multi-planning and large documents does not respect the sort order.
(function() {
"use strict";

const coll = db.sort_big_documents_with_multi_planning;
coll.drop();

function makeDoc(i) {
    return {_id: i, filterKey: 1, num: i, bytes: BinData(0, "A".repeat(13981014) + "==")};
}

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert(makeDoc(i)));
}

// Two possible indexes can answer the query.
assert.commandWorked(coll.createIndex({filterKey: 1, num: 1, foo: 1}));
assert.commandWorked(coll.createIndex({filterKey: 1, num: 1}));

const sortSpec = {
    num: 1
};

const kExpectedNums = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];

{
    // We do a "client side projection," to avoid printing out the massive BinData string if
    // there's an error.
    const nums = [];
    coll.find({filterKey: 1}).sort(sortSpec).forEach(doc => nums.push(doc.num));

    // The results should be in order.
    assert.eq(nums, kExpectedNums);
}

// Same test, but with aggregation.
{
    const nums = [];
    coll.aggregate([{$match: {filterKey: 1}}, {$sort: sortSpec}])
        .forEach(doc => nums.push(doc.num));

    // The results should be in order.
    assert.eq(nums, kExpectedNums);
}
})();
