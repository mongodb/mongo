/**
 * E2E tests for searchSequenceToken metadata inside a $lookup subpipeline containing $search.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = jsTestName() + "_index";

const docs = [
    {_id: 1, title: "cake recipe", test: 100},
    {_id: 2, title: "cake decorating", test: 100},
    {_id: 3, title: "cake baking tips", test: 1},
    {_id: 4, title: "cake frosting guide", test: 10},
    {_id: 5, title: "cake flavors", test: 100},
    {_id: 6, title: "cake layers", test: 100},
    {_id: 7, title: "cake tools", test: 10},
    {_id: 8, title: "cake ingredients", test: 100},
];

describe("search sequence token in $lookup", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));
        createSearchIndex(coll, {
            name: indexName,
            definition: {
                mappings: {
                    dynamic: false,
                    fields: {
                        title: {type: "string"},
                    },
                },
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("returns searchSequenceToken from $search inside $lookup subpipeline", function () {
        const results = coll
            .aggregate([
                {
                    $lookup: {
                        from: coll.getName(),
                        localField: "_id",
                        foreignField: "_id",
                        as: "JoinedIDs",
                        pipeline: [
                            {
                                $search: {
                                    index: indexName,
                                    text: {query: "cake", path: "title"},
                                },
                            },
                            {$limit: 5},
                            {
                                $project: {
                                    _id: 1,
                                    paginationToken: {$meta: "searchSequenceToken"},
                                },
                            },
                        ],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);

        results.forEach((outerDoc) => {
            assert(outerDoc.hasOwnProperty("JoinedIDs"), "missing JoinedIDs", {outerDoc});
            assert.eq(outerDoc.JoinedIDs.length, 1, outerDoc);

            const joined = outerDoc.JoinedIDs[0];
            assert.eq(joined._id, outerDoc._id, outerDoc);
            assert(joined.hasOwnProperty("paginationToken"), "missing paginationToken", {joined});
            assert.eq(typeof joined.paginationToken, "string", joined);
            assert.gt(joined.paginationToken.length, 0, joined);
        });
    });
});
