// Tests that the sort order is obeyed when an aggregation requests sorted results that are
// scattered across multiple shards.
(function() {
    'use strict';

    const shardingTest = new ShardingTest({shards: 2});

    const db = shardingTest.getDB("test");
    const coll = db.sharded_agg_sort;
    coll.drop();

    assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName()}));
    shardingTest.ensurePrimaryShard(db.getName(), 'shard0001');
    assert.commandWorked(
        shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    const nDocs = 10;
    const yValues = [
        "abc",
        "ABC",
        null,
        1,
        NumberLong(2),
        NumberDecimal(-20),
        MinKey,
        MaxKey,
        BinData(0, ""),
        [3, 4],
    ];
    const bulkOp = coll.initializeOrderedBulkOp();
    for (var i = 0; i < nDocs; ++i) {
        bulkOp.insert({_id: i, x: Math.floor(i / 2), y: yValues[i]});
    }
    assert.writeOK(bulkOp.execute());

    // Split the data into 3 chunks
    assert.commandWorked(
        shardingTest.s0.adminCommand({split: coll.getFullName(), middle: {_id: 3}}));
    assert.commandWorked(
        shardingTest.s0.adminCommand({split: coll.getFullName(), middle: {_id: 6}}));

    // Migrate the middle chunk to another shard
    assert.commandWorked(shardingTest.s0.adminCommand({
        movechunk: coll.getFullName(),
        find: {_id: 5},
        to: shardingTest.getOther(shardingTest.getPrimaryShard(db.getName())).name
    }));

    function assertResultsEqual({actual, expected}) {
        const resultsAsString = "  actual: " + tojson(actual) + "\n  expected: " + tojson(expected);
        assert.eq(
            actual.length, expected.length, `different number of results:\n${resultsAsString}`);
        for (let i = 0; i < actual.length; i++) {
            assert.eq(
                actual[i], expected[i], `different results at index ${i}:\n${resultsAsString}`);
        }
    }

    function testSorts() {
        // Test a basic sort by _id.
        assertResultsEqual({
            actual: coll.aggregate([{$sort: {_id: 1}}]).toArray(),
            expected: [
                {_id: 0, x: 0, y: "abc"},
                {_id: 1, x: 0, y: "ABC"},
                {_id: 2, x: 1, y: null},
                {_id: 3, x: 1, y: 1},
                {_id: 4, x: 2, y: NumberLong(2)},
                {_id: 5, x: 2, y: NumberDecimal(-20)},
                {_id: 6, x: 3, y: MinKey},
                {_id: 7, x: 3, y: MaxKey},
                {_id: 8, x: 4, y: BinData(0, "")},
                {_id: 9, x: 4, y: [3, 4]},
            ],
        });
        assertResultsEqual({
            actual: coll.aggregate([{$sort: {_id: 1}}, {$project: {_id: 1}}]).toArray(),
            expected: new Array(nDocs).fill().map(function(_, index) {
                return {_id: index};
            }),
        });

        // Test a compound sort.
        assertResultsEqual({
            actual: coll.aggregate([{$sort: {x: 1, y: 1}}]).toArray(),
            expected: [
                {_id: 1, x: 0, y: "ABC"},
                {_id: 0, x: 0, y: "abc"},
                {_id: 2, x: 1, y: null},
                {_id: 3, x: 1, y: 1},
                {_id: 5, x: 2, y: NumberDecimal(-20)},
                {_id: 4, x: 2, y: NumberLong(2)},
                {_id: 6, x: 3, y: MinKey},
                {_id: 7, x: 3, y: MaxKey},
                {_id: 9, x: 4, y: [3, 4]},
                {_id: 8, x: 4, y: BinData(0, "")},
            ],
        });
        assertResultsEqual({
            actual:
                coll.aggregate([{$sort: {x: 1, y: 1}}, {$project: {_id: 0, x: 1, y: 1}}]).toArray(),
            expected: [
                {x: 0, y: "ABC"},
                {x: 0, y: "abc"},
                {x: 1, y: null},
                {x: 1, y: 1},
                {x: 2, y: NumberDecimal(-20)},
                {x: 2, y: NumberLong(2)},
                {x: 3, y: MinKey},
                {x: 3, y: MaxKey},
                {x: 4, y: [3, 4]},
                {x: 4, y: BinData(0, "")},
            ],
        });

        // Test a compound sort with a missing field.
        assertResultsEqual({
            actual: coll.aggregate({$sort: {missing: -1, x: 1, _id: -1}}).toArray(),
            expected: [
                {_id: 1, x: 0, y: "ABC"},
                {_id: 0, x: 0, y: "abc"},
                {_id: 3, x: 1, y: 1},
                {_id: 2, x: 1, y: null},
                {_id: 5, x: 2, y: NumberDecimal(-20)},
                {_id: 4, x: 2, y: NumberLong(2)},
                {_id: 7, x: 3, y: MaxKey},
                {_id: 6, x: 3, y: MinKey},
                {_id: 9, x: 4, y: [3, 4]},
                {_id: 8, x: 4, y: BinData(0, "")},
            ]
        });
    }
    testSorts();
    assert.commandWorked(coll.createIndex({x: 1}));
    testSorts();
    assert.commandWorked(coll.createIndex({x: 1, y: 1}));
    testSorts();
    assert.commandWorked(coll.createIndex({missing: 1, x: -1}));
    testSorts();
    assert.commandWorked(coll.createIndex({missing: -1, x: 1, _id: -1}));
    testSorts();

    // Test that a sort including the text score is merged properly in a sharded cluster.
    const textColl = db.sharded_agg_sort_text;

    assert.commandWorked(
        shardingTest.s0.adminCommand({shardCollection: textColl.getFullName(), key: {_id: 1}}));

    assert.writeOK(textColl.insert([
        {_id: 0, text: "apple"},
        {_id: 1, text: "apple orange banana apple"},
        {_id: 2, text: "apple orange"},
        {_id: 3, text: "apple orange banana apple apple banana"},
        {_id: 4, text: "apple orange banana"},
        {_id: 5, text: "apple orange banana apple apple"},
    ]));

    // Split the data into 3 chunks
    assert.commandWorked(
        shardingTest.s0.adminCommand({split: textColl.getFullName(), middle: {_id: 2}}));
    assert.commandWorked(
        shardingTest.s0.adminCommand({split: textColl.getFullName(), middle: {_id: 4}}));

    // Migrate the middle chunk to another shard
    assert.commandWorked(shardingTest.s0.adminCommand({
        movechunk: textColl.getFullName(),
        find: {_id: 3},
        to: shardingTest.getOther(shardingTest.getPrimaryShard(db.getName())).name
    }));

    assert.commandWorked(textColl.createIndex({text: "text"}));
    assertResultsEqual({
        actual: textColl
                    .aggregate([
                        {$match: {$text: {$search: "apple banana orange"}}},
                        {$sort: {x: {$meta: "textScore"}}}
                    ])
                    .toArray(),
        expected: [
            {_id: 3, text: "apple orange banana apple apple banana"},
            {_id: 5, text: "apple orange banana apple apple"},
            {_id: 1, text: "apple orange banana apple"},
            {_id: 4, text: "apple orange banana"},
            {_id: 2, text: "apple orange"},
            {_id: 0, text: "apple"},
        ],
    });

    function assertSortedByMetaField(results) {
        for (let i = 0; i < results.length - 1; ++i) {
            assert(results[i].hasOwnProperty("meta"),
                   `Expected all results to have "meta" field, found one without it at index ${i}`);
            assert.gte(
                results[i].meta,
                results[i + 1].meta,
                `Expected results to be sorted by "meta" field, descending. Detected unsorted` +
                    ` results at index ${i}, entire result set: ${tojson(results)}`);
        }
    }

    assertSortedByMetaField(textColl
                                .aggregate([
                                    {$match: {$text: {$search: "apple banana orange"}}},
                                    {$sort: {x: {$meta: "textScore"}}},
                                    {$project: {_id: 0, meta: {$meta: "textScore"}}},
                                ])
                                .toArray());

    assertSortedByMetaField(textColl
                                .aggregate([
                                    {$match: {$text: {$search: "apple banana orange"}}},
                                    {$project: {_id: 0, meta: {$meta: "textScore"}}},
                                    {$sort: {meta: -1}},
                                ])
                                .toArray());

    assertSortedByMetaField(textColl
                                .aggregate([
                                    {$sample: {size: 10}},
                                    {$project: {_id: 0, meta: {$meta: "randVal"}}},
                                ])
                                .toArray());

    shardingTest.stop();
})();
