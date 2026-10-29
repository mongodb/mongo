/**
 * Tests explain for $searchMeta with real mongot.
 * E2E version of jstests/with_mongot/search_mocked/search_meta_explain.js
 * and sharded_search_meta_explain.js.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {verifyE2ESearchMetaExplainOutput} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const coll = db[jsTestName()];
const indexName = jsTestName() + "_index";

const numDocs = 10000;
const startTime = ISODate("2000-01-01T00:00:00.000Z");
const genres = [
    "Drama",
    "Comedy",
    "Romance",
    "Thriller",
    "Documentary",
    "Action",
    "Crime",
    "Adventure",
    "Horror",
    "Biography",
];

const countQuery = {
    "$searchMeta": {
        "index": indexName,
        "range": {"path": "index", "gte": 100, "lt": 9000},
        "count": {"type": "total"},
    },
};

const facetQuery = {
    "$searchMeta": {
        "index": indexName,
        "facet": {
            "operator": {
                "range": {
                    "path": "released",
                    "gte": ISODate("2000-01-01T00:00:00.000Z"),
                    "lte": ISODate("2015-01-31T00:00:00.000Z"),
                },
            },
            "facets": {
                "yearFacet": {
                    "type": "date",
                    "path": "released",
                    "boundaries": [
                        ISODate("2000-01-01"),
                        ISODate("2005-01-01"),
                        ISODate("2010-01-01"),
                        ISODate("2015-01-01"),
                    ],
                    "default": "other",
                },
                "genresFacet": {"type": "string", "path": "genre"},
            },
        },
    },
};

describe("$searchMeta explain", function () {
    before(function () {
        coll.drop();
        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            const time = new Date(startTime.getTime() + i * 1000 * 60 * 60 * 24);
            const genre = genres[i % 10];
            docs.push({_id: i, index: i, released: time, genre: genre});
        }
        assert.commandWorked(coll.insertMany(docs));

        createSearchIndex(coll, {
            name: indexName,
            definition: {
                "mappings": {
                    "dynamic": false,
                    "fields": {
                        "index": {"type": "number"},
                        "released": [{"type": "dateFacet"}, {"type": "date"}],
                        "genre": {"type": "stringFacet"},
                    },
                },
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
        describe(verbosity, function () {
            it("validates explain for count query", function () {
                const result = coll.explain(verbosity).aggregate([countQuery]);
                verifyE2ESearchMetaExplainOutput({
                    explainOutput: result,
                    numFacetBucketsAndCount: 1,
                    verbosity: verbosity,
                });
            });

            it("validates explain for facet query", function () {
                const result = coll.explain(verbosity).aggregate([facetQuery]);
                // 14 facet buckets (10 genre + 4 year) + 1 count = 15
                verifyE2ESearchMetaExplainOutput({
                    explainOutput: result,
                    numFacetBucketsAndCount: 15,
                    verbosity: verbosity,
                });
            });
        });
    }
});
