/**
 * Tests that all search related user functionality (search agg stages and search index commands)
 * are properly rejected on timeseries collections.
 *
 * @tags: [requires_fcv_83, requires_timeseries]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    expectCreateSearchIndexFails,
    expectDropSearchIndexFails,
    expectUpdateSearchIndexFails,
} from "jstests/libs/search.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = jsTestName();
const tsColl = db.getCollection(timeseriesCollName);
assertDropCollection(db, timeseriesCollName);
assert.commandWorked(
    db.createCollection(timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

const nDocs = 10;
const bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {[timeFieldName]: ISODate(), [metaFieldName]: i % 2};
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

(function testSearchStagesDisallowedOnTsCollection() {
    const searchPipelines = [
        [{$search: {index: "default", text: {query: "example", path: metaFieldName}}}],
        [{$vectorSearch: {index: "default", vector: {$meta: "searchVector"}}}],
        [{$searchMeta: {index: "default", text: {query: "example", path: metaFieldName}}}],
        [
            {
                $lookup: {
                    from: timeseriesCollName,
                    let: {lkey: "$key"},
                    pipeline: [
                        {$search: {index: "default", text: {query: "example", path: metaFieldName}}},
                        {$match: {$expr: {$eq: ["$fieldName", "$$lkey"]}}},
                    ],
                    as: "joined",
                },
            },
        ],
        // TODO SERVER-103133 Add tests for $search in a $unionWith.
        // TODO SERVER-103134 Add tests for $search in a $graphLookup.
    ];

    // TODO SERVER-108560 remove the legacy timeseries error codes (10623000 and 40602), once 9.0
    // becomes last LTS.
    // Search stages should fail when querying the timeseries collection directly.
    searchPipelines.forEach((pipeline) => {
        assert.commandFailedWithCode(
            tsColl.runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
            [10557302, 10623000],
            `Expected failure for pipeline: ${tojson(pipeline)}`,
        );
    });

    // Search stages should fail when querying a view on a timeseries collection.
    const viewName = "view_" + timeseriesCollName;
    assert.commandWorked(db.createView(viewName, timeseriesCollName, [{$set: {"view": 100}}]));
    searchPipelines.forEach((pipeline) => {
        assert.commandFailedWithCode(
            db[viewName].runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
            [10557302, 10623000],
            `Expected failure for pipeline: ${tojson(pipeline)}`,
        );
    });

    // All queries on a timeseries collection on a view with $search in the view definition should
    // fail.
    const searchView = "searchview_" + timeseriesCollName;
    assert.commandWorked(
        db.createView(searchView, timeseriesCollName, [
            {$search: {index: "default", text: {query: "example", path: metaFieldName}}},
        ]),
    );
    assert.commandFailedWithCode(
        db[searchView].runCommand("aggregate", {pipeline: [{$match: {}}], cursor: {}}),
        [10557302, 10623000, 40602],
        `Expected failure for pipeline: ${tojson([{$match: {}}])}`,
    );
})();

(function testSearchIndexCommandsDisallowedOnTsCollection() {
    // In this test we confirm the functionality of 4 search index related features.
    //
    // 3 database level commands (expect failure):
    // - createSearchIndex
    // - updateSearchIndex
    // - dropSearchIndex
    //
    // and 1 aggregation stage (expect no results):
    // - $listSearchIndexes
    //
    // Even though '$listSearchIndexes' is a aggregation stage, instead of a search index command,
    // we are testing it with the search index commands as it is in a similar usage category.
    let tsSearchIndexName = "tsSearchIndex";

    // 1. createSearchIndex
    let searchIndexDef = {
        mappings: {dynamic: true, fields: {}},
    };
    expectCreateSearchIndexFails(tsColl, {name: tsSearchIndexName, definition: searchIndexDef}, [10840700, 10840701]);

    // 2. updateSearchIndex
    searchIndexDef.storedSource = {
        exclude: [metaFieldName],
    };
    expectUpdateSearchIndexFails(tsColl, {name: tsSearchIndexName, definition: searchIndexDef}, [10840700, 10840701]);

    // 3. dropSearchIndex
    expectDropSearchIndexFails(tsColl, {name: tsSearchIndexName}, [10840700, 10840701]);

    // 4. $listSearchIndexes (should return an empty array for timeseries collections).
    const results = tsColl.aggregate([{$listSearchIndexes: {}}]).toArray();
    assert.eq(results, [], "Expected no search indexes for timeseries collection");
})();

// Validates that a $search stage works in a subpipeline when the inner collection is non-timeseries, but the outer collection is timeseries.
(function testSearchAllowedInSubpipelineNonTimeseries() {
    // Create a non-timeseries collection.
    const collName = jsTestName() + "_nonts";
    const coll = db[collName];
    coll.drop();
    coll.insert({a: -1, size: "small", [metaFieldName]: 1});
    coll.insert({a: -10, size: "medium", [metaFieldName]: 1});
    coll.insert({a: 100, size: "medium", [metaFieldName]: 2});
    createSearchIndex(coll, {name: "foo-block", definition: {"mappings": {"dynamic": true}}});

    const pipelines = [
        [
            {
                $lookup: {
                    from: collName,
                    let: {lkey: `$${metaFieldName}`},
                    pipeline: [
                        {$search: {index: "foo-block", text: {query: "medium", path: "size"}}},
                        {$match: {$expr: {$eq: [`$${metaFieldName}`, "$$lkey"]}}},
                    ],
                    as: "joined",
                },
            },
        ],
        // TODO SERVER-103133 Add tests for $search in a $unionWith.
        // TODO SERVER-103134 Add tests for $search in a $graphLookup.
    ];

    // TODO SERVER-108560 remove the legacy timeseries error codes (10623000 and 40602), once 9.0
    // becomes last LTS.
    pipelines.forEach((pipeline) => {
        const results = tsColl.aggregate(pipeline).toArray();
        // The 'joined' field should contain the same number of documents as the value of 'tags'.
        results.forEach((doc) => {
            assert.eq(doc.joined.length, doc.tags);
        });
    });

    dropSearchIndex(coll, {name: "foo-block"});
})();
