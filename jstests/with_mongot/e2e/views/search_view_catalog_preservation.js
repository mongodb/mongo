/**
 * Tests that $search and $searchMeta in view pipeline definitions are stored
 * in system.views without desugaring.
 *
 * @tags: [assumes_unsharded_collection]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName() + "_coll";
const coll = testDb.getCollection(collName);

describe("$search view definition catalog preservation", function () {
    const searchIndexName = jsTestName() + "_index";

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, a: 10},
                {_id: 2, a: 0},
                {_id: 3, a: 5},
            ]),
        );
        createSearchIndex(coll, {
            name: searchIndexName,
            definition: {mappings: {dynamic: true}},
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: searchIndexName});
        coll.drop();
    });

    function getPipelineFromViewDef(viewFullName) {
        return testDb.system.views.findOne({_id: viewFullName}).pipeline;
    }

    it("should preserve raw $search in system.views after creation and query", function () {
        const viewName = collName + "_search_view";
        const searchQuery = {
            index: searchIndexName,
            exists: {path: "_id"},
        };
        testDb.getCollection(viewName).drop();
        assert.commandWorked(testDb.createView(viewName, collName, [{$search: searchQuery}]));
        const searchView = testDb.getCollection(viewName);

        // Query the view to trigger desugaring in the execution path.
        const results = searchView.find().toArray();
        assert.gt(results.length, 0, "expected at least one result from the view");

        // Verify system.views stores the raw $search pipeline, not desugared form.
        assert.eq([{$search: searchQuery}], getPipelineFromViewDef(searchView.getFullName()));

        testDb.getCollection(viewName).drop();
    });

    it("should preserve raw $searchMeta in system.views after creation", function () {
        const viewName = collName + "_search_meta_view";
        const searchQuery = {
            index: searchIndexName,
            exists: {path: "_id"},
        };
        testDb.getCollection(viewName).drop();
        assert.commandWorked(testDb.createView(viewName, collName, [{$searchMeta: searchQuery}]));
        const searchMetaView = testDb.getCollection(viewName);

        assert.eq(
            [{$searchMeta: searchQuery}],
            getPipelineFromViewDef(searchMetaView.getFullName()),
        );

        testDb.getCollection(viewName).drop();
    });
});
