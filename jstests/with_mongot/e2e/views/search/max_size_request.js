/**
 * This test builds a pipeline nearing the BSON size limit and attempts to create search indexes
 * with parameters that would put the request over the limit. This is to ensure that our search
 * index interface correctly catches and returns such errors.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex} from "jstests/libs/search.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

// This test is only run once (not with both storedSource values) since the BSON size limit behavior
// is independent of the storedSource setting.
const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "foo", category: "test"});
assert.commandWorked(bulk.execute());

const parentName = "underlyingSourceCollection";

// 15.9 MB target size for the pipeline (just under 16 MB BSON limit).
const targetSize = 15 * 1024 * 1024 + 1024 * 200;

// 100 KB string to append to the pipeline.
const largeString = "a".repeat(1024 * 100);

const fieldNamePrefix = "large_field_";
let fieldNum = 0;
let pipeline = [];

for (let currentSize = 0; currentSize < targetSize;) {
    // Add large stage to pipeline.
    const stage = {$addFields: {[`${fieldNamePrefix}${fieldNum}`]: largeString}};
    pipeline.push(stage);

    // Increment variables.
    fieldNum++;
    currentSize += JSON.stringify(stage).length;
}

// At this point, the pipeline is ~15.9 MB. There is an additional 800 KB needed to overflow BSON
// (16 MB is actually 16.7MB).
const viewName = "viewName";
assert.commandWorked(testDb.createView(viewName, parentName, pipeline));
const view = testDb[viewName];

const indexConfig = {
    coll: view,
    definition: {name: "viewNameIndex", definition: {"mappings": {"dynamic": true}}}
};

const maxSizeRequestTestCases = (isStoredSource) => {
    // Error case 1: Index name too large.
    assert.throwsWithCode(() => createSearchIndex(view, {
                              name: "tooLargeNameIndex".repeat(1024 * 52),
                              definition: {"mappings": {"dynamic": true}}
                          }),
                          ErrorCodes.BSONObjectTooLarge);

    // Error case 2: Index definition too large.
    assert.throwsWithCode(
        () => createSearchIndex(view, {
            name: "tooLargeDefinitionIndex",
            definition: {
                "mappings": {"dynamic": true},
                "fields": {"large_metadata": {"type": "string", "meta": "b".repeat(1024 * 800)}}
            }
        }),
        ErrorCodes.BSONObjectTooLarge);

    // Ensure that a simple query can be run on the successful search index after the expected
    // failures.
    const normalSearchQuery = [{
        $search: {
            index: "viewNameIndex",
            text: {query: "test", path: "category"},
            returnStoredSource: isStoredSource
        }
    }];

    // We can only assert that the view is applied correctly for the single_node suite and
    // single_shard suite (numberOfShardsForCollection() will return 1 in a single node
    // environment). This is because the sharded_cluster suite will omit the explain output for
    // each shard in this test because the view definition is too large (but still small enough
    // to run queries on).
    if (FixtureHelpers.numberOfShardsForCollection(coll) == 1) {
        validateSearchExplain(view, normalSearchQuery, isStoredSource, pipeline);
    } else {
        // For sharded clusters, still run the query but don't validate the explain plan.
        validateSearchExplain(view, normalSearchQuery, isStoredSource);

        const results = view.aggregate(normalSearchQuery).toArray();
        assert.eq(results.length, 1, "Query should return exactly one result");
    }
};

createSearchIndexesAndExecuteTests(indexConfig, maxSizeRequestTestCases);
