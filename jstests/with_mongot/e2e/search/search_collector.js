/**
 * Verify that `$search` sets the '$$SEARCH_META' variable with real mongot. Metadata is produced
 * via the `count` operator (the mocked version of this test injected arbitrary metadata values).
 *
 * The sharded e2e passthrough suites give this file the coverage that the mocked
 * search_collector_sharded_* variants set up by hand.
 * E2E version of jstests/with_mongot/search_mocked/search_collector.js and its
 * search_collector_* topology variants.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = "search_collector_index";
// Matches all 3 documents; the count operator makes real mongot return SEARCH_META.
const searchQuery = {
    index: indexName,
    exists: {path: "title"},
    count: {type: "total"},
};
const expectedMeta = {count: {total: NumberLong(3)}};

describe("$search sets $$SEARCH_META", function () {
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

    it("should expose the same metadata to every result document", function () {
        const results = coll
            .aggregate([{$search: searchQuery}, {$project: {_id: 1, meta: "$$SEARCH_META"}}])
            .toArray();

        assertArrayEq({
            actual: results,
            expected: [
                {_id: 1, meta: expectedMeta},
                {_id: 2, meta: expectedMeta},
                {_id: 3, meta: expectedMeta},
            ],
        });
    });

    it("should evaluate unset metadata fields to missing", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, missingMeta: "$$SEARCH_META.missing"}},
            ])
            .toArray();

        assertArrayEq({actual: results, expected: [{_id: 1}, {_id: 2}, {_id: 3}]});
    });

    it("should work with a stage that forces a merge", function () {
        // The merge half of $group cannot run on the shards, so in sharded topologies this
        // exercises SEARCH_META surviving the split into a merging pipeline. (The mocked original
        // used $out, which the sharded-collections passthrough disallows: the implicitly sharded
        // output collection cannot be an $out target.)
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                {$group: {_id: "$meta", ids: {$addToSet: "$_id"}}},
            ])
            .toArray();

        assert.eq(results.length, 1, results);
        assert.eq(results[0]._id, expectedMeta, results);
        assertArrayEq({actual: results[0].ids, expected: [1, 2, 3]});
    });
});
