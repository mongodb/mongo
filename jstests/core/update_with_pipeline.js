/**
 * Tests execution of pipeline-style update.
 *
 * 'requires_find_command' needed to prevent this test from running with 'compatibility' write mode
 * as pipeline-style update is not supported by OP_UPDATE.
 *
 * @tags: [requires_find_command, requires_non_retryable_writes]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

    const collName = "update_with_pipeline";
    const coll = db[collName];

    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({"y.$**": 1}));

    /**
     * Confirms that an update returns the expected set of documents. 'nModified' documents from
     * 'resultDocList' must match. 'nModified' may be smaller then the number of elements in
     * 'resultDocList'. This allows for the case where there are multiple documents that could be
     * updated, but only one is actually updated due to a 'multi: false' argument.
     */
    function testUpdate(
        {query, initialDocumentList, update, resultDocList, nModified, options = {}}) {
        assert.eq(initialDocumentList.length, resultDocList.length);
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.insert(initialDocumentList));
        const res = assert.commandWorked(coll.update(query, update, options));
        assert.eq(nModified, res.nModified);

        let nMatched = 0;
        for (let i = 0; i < resultDocList.length; ++i) {
            if (0 === bsonWoCompare(coll.findOne(resultDocList[i]), resultDocList[i])) {
                ++nMatched;
            }
        }
        assert.eq(nModified, nMatched);
    }

    function testUpsertDoesInsert(query, update, resultDoc) {
        assert.commandWorked(coll.remove({}));
        assert.commandWorked(coll.update(query, update, {upsert: true}));
        assert.eq(coll.findOne({}), resultDoc, coll.find({}).toArray());
    }

    // Update with existing document.
    testUpdate({
        query: {_id: 1},
        initialDocumentList: [{_id: 1, x: 1}],
        update: [{$set: {foo: 4}}],
        resultDocList: [{_id: 1, x: 1, foo: 4}],
        nModified: 1
    });
    testUpdate({
        query: {_id: 1},
        initialDocumentList: [{_id: 1, x: 1, y: 1}],
        update: [{$project: {x: 1}}],
        resultDocList: [{_id: 1, x: 1}],
        nModified: 1
    });
    testUpdate({
        query: {_id: 1},
        initialDocumentList: [{_id: 1, x: 1, t: {u: {v: 1}}}],
        update: [{$replaceWith: "$t"}],
        resultDocList: [{_id: 1, u: {v: 1}}],
        nModified: 1
    });

    // Multi-update.
    testUpdate({
        query: {x: 1},
        initialDocumentList: [{_id: 1, x: 1}, {_id: 2, x: 1}],
        update: [{$set: {bar: 4}}],
        resultDocList: [{_id: 1, x: 1, bar: 4}, {_id: 2, x: 1, bar: 4}],
        nModified: 2,
        options: {multi: true}
    });

    // This test will fail in a sharded cluster when the 2 initial documents live on different
    // shards.
    if (!FixtureHelpers.isMongos(db)) {
        testUpdate({
            query: {_id: {$in: [1, 2]}},
            initialDocumentList: [{_id: 1, x: 1}, {_id: 2, x: 2}],
            update: [{$set: {bar: 4}}],
            resultDocList: [{_id: 1, x: 1, bar: 4}, {_id: 2, x: 2, bar: 4}],
            nModified: 1,
            options: {multi: false}
        });
    }

    // Upsert performs insert.
    testUpsertDoesInsert({_id: 1, x: 1}, [{$set: {foo: 4}}], {_id: 1, x: 1, foo: 4});
    testUpsertDoesInsert({_id: 1, x: 1}, [{$project: {x: 1}}], {_id: 1, x: 1});
    testUpsertDoesInsert({_id: 1, x: 1}, [{$project: {x: "foo"}}], {_id: 1, x: "foo"});

    // Update fails when invalid stage is specified. This is a sanity check rather than an
    // exhaustive test of all stages.
    assert.commandFailedWithCode(coll.update({x: 1}, [{$match: {x: 1}}]),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({x: 1}, [{$sort: {x: 1}}]), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({x: 1}, [{$facet: {a: [{$match: {x: 1}}]}}]),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({x: 1}, [{$indexStats: {}}]),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({x: 1}, [{
                                                 $bucket: {
                                                     groupBy: "$a",
                                                     boundaries: [0, 1],
                                                     default: "foo",
                                                     output: {count: {$sum: 1}}
                                                 }
                                             }]),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        coll.update({x: 1},
                    [{$lookup: {from: "foo", as: "as", localField: "a", foreignField: "b"}}]),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({x: 1}, [{
                                                 $graphLookup: {
                                                     from: "foo",
                                                     startWith: "$a",
                                                     connectFromField: "a",
                                                     connectToField: "b",
                                                     as: "as"
                                                 }
                                             }]),
                                 ErrorCodes.InvalidOptions);

    // Update fails when supported agg stage is specified outside of pipeline.
    assert.commandFailedWithCode(coll.update({_id: 1}, {$addFields: {x: 1}}),
                                 ErrorCodes.FailedToParse);

    // The 'arrayFilters' option is not valid for pipeline updates.
    assert.commandFailedWithCode(
        coll.update({_id: 1}, [{$set: {x: 1}}], {arrayFilters: [{x: {$eq: 1}}]}),
        ErrorCodes.FailedToParse);
})();
