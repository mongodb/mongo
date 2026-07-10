/**
 * Verify that `$searchBeta` works as an alias for `$search` with real mongot.
 * E2E version of jstests/with_mongot/search_mocked/search_beta.js
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = "search_beta_index";
const searchQuery = {index: indexName, text: {query: "cakes", path: "title"}};

describe("$searchBeta", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 1, title: "cakes"},
                {_id: 2, title: "cookies and cakes"},
                {_id: 3, title: "vegetables"},
            ]),
        );

        createSearchIndex(coll, {
            name: indexName,
            definition: {mappings: {dynamic: false, fields: {title: {type: "string"}}}},
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("should parse on a nonexistent collection", function () {
        const cursor = db
            .getCollection(collName + "_does_not_exist")
            .aggregate([{$searchBeta: searchQuery}]);
        assert.eq(cursor.toArray(), []);
    });

    it("should return the same results as $search", function () {
        const searchBetaResults = coll.aggregate([{$searchBeta: searchQuery}]).toArray();
        const searchResults = coll.aggregate([{$search: searchQuery}]).toArray();
        assertArrayEq({actual: searchBetaResults, expected: searchResults});
        assertArrayEq({
            actual: searchBetaResults,
            expected: [
                {_id: 1, title: "cakes"},
                {_id: 2, title: "cookies and cakes"},
            ],
        });
    });
});
