/**
 * Allow a view defined by $search as a $lookup foreign namespace.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_82 ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {getMovieData, getMovieSearchIndexSpec} from "jstests/with_mongot/e2e_lib/data/movies.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany(getMovieData()));

const indexName = "movie_search_index";
createSearchIndex(coll, {name: indexName, definition: getMovieSearchIndexSpec().definition});

const searchViewName = jsTestName() + "_search_view";
assert.commandWorked(
    db.createView(searchViewName, coll.getName(), [
        {$search: {index: indexName, text: {query: "ape", path: ["fullplot", "title"]}}},
    ]),
);

const expected = db[searchViewName].aggregate([]).toArray();
assert.gt(expected.length, 0, "expected the $search view to materialize some documents");
const expectedIds = new Set(expected.map((d) => d._id));

{
    const res = coll
        .aggregate([{$lookup: {from: searchViewName, as: "m", pipeline: []}}])
        .toArray();
    assert.eq(res.length, getMovieData().length, "left join must preserve every local doc");
    for (const doc of res) {
        const gotIds = new Set(doc.m.map((d) => d._id));
        assert.eq(
            gotIds,
            expectedIds,
            () => `join must equal the materialized view for ${doc._id}`,
        );
    }
}

{
    const res = coll
        .aggregate([
            {$lookup: {from: searchViewName, as: "m", localField: "_id", foreignField: "_id"}},
        ])
        .toArray();
    for (const doc of res) {
        for (const md of doc.m) {
            assert.eq(md._id, doc._id, () => `foreign match must share _id for ${doc._id}`);
            assert(expectedIds.has(md._id), () => `match must be a search result: ${md._id}`);
        }
        const expectedMatch = expectedIds.has(doc._id) ? [doc._id] : [];
        assert.sameMembers(
            doc.m.map((md) => md._id),
            expectedMatch,
            () => `equality lookup result should match the search view for ${doc._id}`,
        );
    }
}

dropSearchIndex(coll, {name: indexName});
