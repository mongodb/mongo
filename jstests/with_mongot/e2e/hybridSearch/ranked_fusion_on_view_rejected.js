/**
 * Tests that $rankFusion can't be used on a view namespace if the namespace is timeseries, or if
 * the $rankFusion query has mongot input pipelines.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(coll.getName()));

const searchIndexName = "searchIndex";
createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});
const vectorSearchIndexName = "vectorSearchIndex";
createSearchIndex(coll, {
    name: vectorSearchIndexName,
    type: "vectorSearch",
    definition: {
        "fields": [{
            "type": "vector",
            "numDimensions": 1536,
            "path": "plot_embedding",
            "similarity": "euclidean"
        }]
    }
});

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

const matchViewName = "match_view";
const matchViewPipeline = {
    $match: {a: "foo"}
};

const geoNearViewName = "geo_near_view";
const geoNearViewPipeline = {
    $geoNear: {near: {type: "Point", coordinates: [0, 0]}, key: "loc", spherical: true}
};

const rankFusionPipeline = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$sort: {x: -1}}],
                b: [{$sort: {x: 1}}],
            }
        }
    }
}];

const rankFusionPipelineWithSearchFirst = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$search: {index: searchIndexName, text: {query: "fo", path: "a"}}}],
                b: [{$sort: {x: 1}}],
            }
        }
    }
}];

const rankFusionPipelineWithSearchSecond = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$sort: {x: -1}}],
                b: [{$search: {index: searchIndexName, text: {query: "fo", path: "a"}}}],
            }
        }
    }
}];

const rankFusionPipelineOnlyOneVectorSearch = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{
                    $vectorSearch: {
                        queryVector: null,
                        path: "plot_embedding",
                        exact: true,
                        index: vectorSearchIndexName,
                        limit: 10,
                    }
                }],
            }
        }
    }
}];

// TODO SERVER-103504: Move the mongot queries to the allowed test.
// Create a view with $match.
assert.commandWorked(db.createView(matchViewName, coll.getName(), [matchViewPipeline]));

// Create a view with $geoNear.
assert.commandWorked(db.createView(geoNearViewName, coll.getName(), [geoNearViewPipeline]));

// Running a $rankFusion with mongot subpipelines fails if the query is on a view.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: matchViewName, pipeline: rankFusionPipelineWithSearchSecond, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: matchViewName, pipeline: rankFusionPipelineWithSearchFirst, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: matchViewName, pipeline: rankFusionPipelineOnlyOneVectorSearch, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);

// TODO SERVER-105682: Add tests for $geoNear in the input pipelines.
// TODO SERVER-105862: Move this test to the allowed test.
assert.commandFailedWithCode(
    db.runCommand({aggregate: geoNearViewName, pipeline: rankFusionPipeline, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);

// Now test on a timeseries collection (which is modeled as a view under-the-hood).
const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = "rank_fusion_timeseries";

assert.commandWorked(db.createCollection(
    timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
const tsColl = db.getCollection(timeseriesCollName);

const nDocs = 50;
const bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {loc: [40, 40], descr: i.toString()},
        value: i + nDocs,
    };
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

// Running $rankFusion on timeseries collection is disallowed.
assert.commandFailedWithCode(
    tsColl.runCommand("aggregate", {pipeline: rankFusionPipeline, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);

dropSearchIndex(coll, {name: searchIndexName});
