/**
 * Tests that the $sort stage performs sorts correctly, whether in-memory, merging on mongos, or
 * merging on a shard. (The sharding scenarios are tested when this test is run in the
 * aggregation_sharded_collections_passthrough.)
 */
(function() {
    "use strict";

    Random.setRandomSeed();
    const coll = db.getCollection("collation_sort_japanese");

    // In Japanese, the order of vowels is a, i, u, e, o. The sorting of mixed katakana and hiragana
    // vowels differs depending on the collation:
    //
    //  - With the simple collation, hiragana vowels come first (in order), followed by katakana.
    //  - In the Japanese locale, vowels with the same sound sort together. Whether hiragana or
    //    katakana comes first depends on the strength level of the collation.
    const data = [
        {kana: "ア", val: 0, name: "katakana a"},
        {kana: "イ", val: 1, name: "katakana i"},
        {kana: "ウ", val: 2, name: "katakana u"},
        {kana: "エ", val: 3, name: "katakana e"},
        {kana: "オ", val: 4, name: "katakana o"},
        {kana: "あ", val: 5, name: "hiragana a"},
        {kana: "い", val: 6, name: "hiragana i"},
        {kana: "う", val: 7, name: "hiragana u"},
        {kana: "え", val: 8, name: "hiragana e"},
        {kana: "お", val: 9, name: "hiragana o"},
    ];

    const simpleCollation = {locale: "simple"};
    const jaCollationStr3 = {locale: "ja"};
    const jaCollationStr4 = {locale: "ja", strength: 4};

    /**
     * Inserts each doc of 'docs' into the collection in no specified order before running tests.
     */
    function runTests(docs) {
        let bulk = coll.initializeUnorderedBulkOp();
        for (let doc of docs) {
            bulk.insert(doc);
        }
        assert.writeOK(bulk.execute());

        let sortOrder;

        function assertAggregationSortOrder(collation, expectedVals) {
            let expectedDocs = expectedVals.map(val => ({val: val}));
            let result = coll.aggregate([{$sort: sortOrder}, {$project: {_id: 0, val: 1}}],
                                        {collation: collation})
                             .toArray();
            assert.eq(result,
                      expectedDocs,
                      "sort returned wrong order with sort pattern " + tojson(sortOrder) +
                          " and collation " + tojson(collation));

            // Run the same aggregation, but in a sharded cluster, force the merging to be performed
            // on a shard instead of on mongos.
            result = coll.aggregate(
                             [
                               {$_internalSplitPipeline: {mergeType: "anyShard"}},
                               {$sort: sortOrder},
                               {$project: {_id: 0, val: 1}}
                             ],
                             {collation: collation})
                         .toArray();
            assert.eq(result,
                      expectedDocs,
                      "sort returned wrong order with sort pattern " + tojson(sortOrder) +
                          " and collation " + tojson(collation) + " when merging on a shard");
        }

        // Start with a sort on a single key.
        sortOrder = {kana: 1};

        // With the binary collation, hiragana codepoints sort before katakana codepoints.
        assertAggregationSortOrder(simpleCollation, [5, 6, 7, 8, 9, 0, 1, 2, 3, 4]);

        // With the Japanese collation at strength 4, a hiragana codepoint always sorts before its
        // equivalent katakana.
        assertAggregationSortOrder(jaCollationStr4, [5, 0, 6, 1, 7, 2, 8, 3, 9, 4]);

        // Test a sort on a compound key.
        sortOrder = {kana: 1, val: 1};

        // With the binary collation, hiragana codepoints sort before katakana codepoints.
        assertAggregationSortOrder(simpleCollation, [5, 6, 7, 8, 9, 0, 1, 2, 3, 4]);

        // With the default Japanese collation, hiragana and katakana with the same pronunciation
        // sort together but with no specified order. The compound sort on "val" breaks the tie and
        // puts the katakana first.
        assertAggregationSortOrder(jaCollationStr3, [0, 5, 1, 6, 2, 7, 3, 8, 4, 9]);

        // With the Japanese collation at strength 4, a hiragana codepoint always sorts before its
        // equivalent katakana.
        assertAggregationSortOrder(jaCollationStr4, [5, 0, 6, 1, 7, 2, 8, 3, 9, 4]);
    }

    // Test sorting documents with only scalar values.
    coll.drop();
    runTests(data);

    // Test sorting documents containing singleton arrays.
    assert(coll.drop());
    runTests(data.map(doc => {
        let copy = Object.extend({}, doc);
        copy.kana = [copy.kana];
        return copy;
    }));

    // Test sorting documents containing arrays with multiple elements.
    assert(coll.drop());
    runTests(data.map(doc => {
        let copy = Object.extend({}, doc);
        copy.kana = [copy.kana, copy.kana, copy.kana];
        return copy;
    }));

    // Test sorting documents where some values are scalars and others are arrays.
    assert(coll.drop());
    runTests(data.map(doc => {
        let copy = Object.extend({}, doc);
        if (Math.random() < 0.5) {
            copy.kana = [copy.kana];
        }
        return copy;
    }));

    // Create indexes that provide sorts and assert that the results are equivalent.
    assert(coll.drop());
    assert.commandWorked(
        coll.createIndex({kana: 1}, {name: "k1_jaStr3", collation: jaCollationStr3}));
    assert.commandWorked(
        coll.createIndex({kana: 1}, {name: "k1_jaStr4", collation: jaCollationStr4}));
    assert.commandWorked(
        coll.createIndex({kana: 1, val: 1}, {name: "k1v1_jaStr3", collation: jaCollationStr3}));
    assert.commandWorked(
        coll.createIndex({kana: 1, val: 1}, {name: "k1v1_jaStr4", collation: jaCollationStr4}));
    runTests(data.map(doc => {
        let copy = Object.extend({}, doc);
        if (Math.random() < 0.5) {
            copy.kana = [copy.kana];
        }
        return copy;
    }));
}());
