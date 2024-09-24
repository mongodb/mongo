/**
 * Tests explain for $searchMeta.
 * @tags: [
 * featureFlagSearchExplainExecutionStats,
 * requires_mongot_1_39
 * ]
 */
import {verifyE2ESearchMetaExplainOutput} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const coll = db[jsTestName()];
coll.drop();
const numDocs = 10000;
let docs = [];

const startTime = ISODate("2000-01-01T00:00:00.000Z");
let genres = [
    "Drama",
    "Comedy",
    "Romance",
    "Thriller",
    "Documentary",
    "Action",
    "Crime",
    "Adventure",
    "Horror",
    "Biography"
];
for (let i = 0; i < numDocs; i++) {
    // We increment by one day each time.
    const time = new Date(startTime.getTime() + (i * 1000 * 60 * 60 * 24));
    const genre = genres[i % 10];
    docs.push({_id: i, index: i, released: time, genre: genre});
}
assert.commandWorked(coll.insertMany(docs));
coll.createSearchIndex({
    name: "facet-index",
    definition: {
        "mappings": {
            "dynamic": false,
            "fields": {
                "index": {"type": "number"},
                "released": [{"type": "dateFacet"}, {"type": "date"}],
                "genre": {"type": "stringFacet"}
            }
        }
    }
});

const countQuery = {
    "$searchMeta": {
        "index": "facet-index",
        "range": {"path": "index", "gte": 100, "lt": 9000},
        "count": {"type": "total"}
    }
};

const facetQuery = {
    "$searchMeta": {
        "index": "facet-index",
        "facet": {
            "operator": {
                "range": {
                    "path": "released",
                    "gte": ISODate("2000-01-01T00:00:00.000Z"),
                    "lte": ISODate("2015-01-31T00:00:00.000Z")
                }
            },
            "facets": {
                "yearFacet": {
                    "type": "date",
                    "path": "released",
                    "boundaries": [
                        ISODate("2000-01-01"),
                        ISODate("2005-01-01"),
                        ISODate("2010-01-01"),
                        ISODate("2015-01-01")
                    ],
                    "default": "other"
                },
                "genresFacet": {"type": "string", "path": "genre"}
            },
        },
    }
};

function runExplainTest(verbosity) {
    // Count Query Test.
    let result = coll.explain(verbosity).aggregate([countQuery]);
    verifyE2ESearchMetaExplainOutput(
        {explainOutput: result, numFacetBucketsAndCount: 1, verbosity: verbosity});

    // Facet Query Test.
    result = coll.explain(verbosity).aggregate([facetQuery]);
    // numFacetBucketsAndCount is 15, 14 from facet buckets (10 genre, 4 year) and 1 from count
    verifyE2ESearchMetaExplainOutput(
        {explainOutput: result, numFacetBucketsAndCount: 15, verbosity: verbosity});
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");
