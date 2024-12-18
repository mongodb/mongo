/**
 * Tests that the 'searchScoreDetails' metadata field is also accessible by the 'scoreDetails'
 * metadata field.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany([
    {floor: 0, storeName: "Bob's Burger"},
    {floor: 1, storeName: "Burgers Galore"},
    {floor: 2, storeName: "Hot Dog Stand"},
    {floor: 3, storeName: "Burger Wanna Burger"}
]));

const indexName = "mall_stores_index";
createSearchIndex(coll, {name: indexName, definition: {"mappings": {"dynamic": true}}});

const searchQuery = {
    index: indexName,
    text: {query: "Burger", path: ["storeName"]},
    scoreDetails: true
};

let results = coll.aggregate([
                      {$search: searchQuery},
                      {
                          $project: {
                              scoreDetails: {$meta: "scoreDetails"},
                              searchScoreDetails: {$meta: "searchScoreDetails"}
                          }
                      }
                  ])
                  .toArray();
assert.eq(results.length, 2);

// "scoreDetails" and "searchScoreDetails" should have the same contents populated by whatever
// details were returned from mongot.
for (let result of results) {
    assert(result.hasOwnProperty("scoreDetails"));
    assert(result.hasOwnProperty("searchScoreDetails"));
    assert.eq(result["scoreDetails"], result["searchScoreDetails"]);
}

dropSearchIndex(coll, {name: indexName});
