/**
 * This is a property-based test that stresses the validation of $meta field references. Some (but
 * not all) $meta fields will be rejected at parse time if referenced in a query where said metadata
 * is not produced (like if "textScore" is referenced without $text). This test strives to confirm
 * the behavior per field: for example, that fields that do get validated will only succeed if the
 * metadata is produced, or that fields that do not get validated will always succeed.
 *
 * It works by generating a pipeline with two stages: first a stage that may or may not produce
 * relevant metadata fields (for example, $geoNear produces "geoNearDistance" and "geoNearPoint"
 * metadata), then a stage that tries to reference a metadata field. We verify based on a set of
 * rules if the reference to the metadata value should pass validation or fail.
 *
 * TODO SERVER-100443 Expand on this test by adding more complex pipeline shapes with at least 3
 * stages.
 *
 * featureFlagRankFusionFull is required to enable use of "score".
 * featureFlagSearchHybridScoringFull is required to enable use of $score.
 * The $rankFusion feature flag is required to enable use of "score" and "searchScore".
 * @tags: [
 *   featureFlagRankFusionBasic,
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 * ]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();
const isSharded = FixtureHelpers.isSharded(coll);

assert.commandWorked(coll.insertMany([
    {
        _id: 0,
        a: 3,
        textField: "three blind mice",
        geoField: [23, 51],
        vector: [0.006585975, -0.03453151, -0.0073834695, -0.032803606, -0.0032448056]
    },
    {
        _id: 1,
        a: 2,
        textField: "the three stooges",
        geoField: [25, 49],
        vector: [0.015856847, -0.0032713888, 0.011949126, -0.0062968833, 0.0032148995]
    },
    {
        _id: 2,
        a: 3,
        textField: "we three kings",
        geoField: [30, 51],
        vector: [0.0071708043, 0.0016248949, 0.014487816, -0.000010448943, 0.027673058]
    }
]));
assert.commandWorked(coll.createIndex({textField: "text"}));
assert.commandWorked(coll.createIndex({geoField: "2d"}));
assert.commandWorked(
    createSearchIndex(coll, {name: "search-index", definition: {mappings: {dynamic: true}}}));
assert.commandWorked(createSearchIndex(coll, {
    name: "vector-search-index",
    type: "vectorSearch",
    definition:
        {fields: [{type: "vector", numDimensions: 5, path: "vector", similarity: "euclidean"}]}
}));

const kUnavailableMetadataErrCode = 40218;

const searchStage = {
    $search: {index: "search-index", exists: {path: "textField"}}
};

// The set of stages that may be generated as the initial stage of the pipeline.
const FirstStageOptions = Object.freeze({
    GEO_NEAR: {$geoNear: {near: [25, 25]}},
    FTS_MATCH: {$match: {$text: {$search: "three"}}},
    NON_FTS_MATCH: {$match: {a: {$gt: 3}}},
    SEARCH: searchStage,
    SEARCH_W_DETAILS:
        {$search: {index: "search-index", exists: {path: "textField"}, scoreDetails: true}},
    VECTOR_SEARCH: {
        $vectorSearch: {
            queryVector: [-0.012674698, 0.013308106, -0.005494981, -0.008286549, -0.00289768],
            path: "vector",
            exact: true,
            index: "vector-search-index",
            limit: 10
        }
    },
    RANK_FUSION: {$rankFusion: {input: {pipelines: {search: [searchStage]}}}},
    RANK_FUSION_W_DETAILS:
        {$rankFusion: {input: {pipelines: {search: [searchStage]}}, scoreDetails: true}},
    SORT: {$sort: {a: -1}},
    SCORE: {$score: {score: {$divide: [1, "$a"]}}}
});

// The set of metadata fields that can be referenced inside $meta, alongside information used to
// dictate if the queries should succeed or not.
const MetaFields = Object.freeze({
    TEXT_SCORE: {
        name: "textScore",
        shouldBeValidated: true,
        debugName: "text score",
        validSortKey: true,
        firstStageRequired: [FirstStageOptions.FTS_MATCH]
    },
    GEO_NEAR_DIST: {
        name: "geoNearDistance",
        shouldBeValidated: true,
        debugName: "$geoNear distance",
        validSortKey: true,
        firstStageRequired: [FirstStageOptions.GEO_NEAR]
    },
    GEO_NEAR_PT: {
        name: "geoNearPoint",
        shouldBeValidated: true,
        debugName: "$geoNear point",
        validSortKey: false,
        firstStageRequired: [FirstStageOptions.GEO_NEAR]
    },
    RAND_VAL:
        {name: "randVal", shouldBeValidated: false, debugName: "rand val", validSortKey: true},
    SEARCH_SCORE: {
        name: "searchScore",
        shouldBeValidated: true,
        debugName: "$search score",
        validSortKey: true,
        firstStageRequired: [
            FirstStageOptions.SEARCH,
            FirstStageOptions.SEARCH_W_DETAILS,
            // TODO SERVER-100443: $rankFusion shouldn't produce
            // "searchScore" metadata, but it thinks it will if one
            // of the $rankFusion input pipelines has $search.
            FirstStageOptions.RANK_FUSION,
            FirstStageOptions.RANK_FUSION_W_DETAILS
        ]
    },
    SEARCH_HIGHLIGHTS: {
        name: "searchHighlights",
        shouldBeValidated: false,
        debugName: "$search highlights",
        validSortKey: false
    },
    RECORD_ID:
        {name: "recordId", shouldBeValidated: false, debugName: "record ID", validSortKey: false},
    INDEX_KEY:
        {name: "indexKey", shouldBeValidated: false, debugName: "index key", validSortKey: false},
    SORT_KEY: {
        name: "sortKey",
        shouldBeValidated: true,
        debugName: "sortKey",
        validSortKey: false,
        firstStageRequired: [
            FirstStageOptions.SORT,
            FirstStageOptions.GEO_NEAR,
            FirstStageOptions.SCORE,
            FirstStageOptions.SEARCH,
            FirstStageOptions.SEARCH_W_DETAILS,
            FirstStageOptions.RANK_FUSION,
            FirstStageOptions.RANK_FUSION_W_DETAILS,
            FirstStageOptions.VECTOR_SEARCH,
        ]
    },
    SEARCH_SCORE_DETAIS: {
        name: "searchScoreDetails",
        shouldBeValidated: false,
        debugName: "$search score details",
        validSortKey: false
    },
    SEARCH_SEQUENCE_TOKEN: {
        name: "searchSequenceToken",
        shouldBeValidated: false,
        debugName: "$search sequence token",
        validSortKey: false
    },
    TIMESERIES_BUCKET_MIN_TIME: {
        name: "timeseriesBucketMinTime",
        shouldBeValidated: false,
        debugName: "timeseries bucket min time",
        validSortKey: false
    },
    TIMESERIES_BUCKET_MAX_TIME: {
        name: "timeseriesBucketMaxTime",
        shouldBeValidated: false,
        debugName: "timeseries bucket max time",
        validSortKey: false
    },
    VECTOR_SEARCH_SCORE: {
        name: "vectorSearchScore",
        shouldBeValidated: true,
        debugName: "$vectorSearch distance",
        validSortKey: true,
        firstStageRequired: [FirstStageOptions.VECTOR_SEARCH]
    },
    SCORE: {
        name: "score",
        shouldBeValidated: true,
        debugName: "score",
        validSortKey: true,
        firstStageRequired: [
            FirstStageOptions.FTS_MATCH,
            FirstStageOptions.SCORE,
            FirstStageOptions.VECTOR_SEARCH,
            FirstStageOptions.SEARCH,
            FirstStageOptions.SEARCH_W_DETAILS,
            FirstStageOptions.RANK_FUSION,
            FirstStageOptions.RANK_FUSION_W_DETAILS,
        ]
    },
    SCORE_DETAILS: {
        name: "scoreDetails",
        shouldBeValidated: true,
        debugName: "scoreDetails",
        validSortKey: false,
        firstStageRequired: [
            FirstStageOptions.SEARCH_W_DETAILS,
            FirstStageOptions.RANK_FUSION_W_DETAILS,
        ]
    }
});

// Each test case chooses one arbitrary from the FirstStageOptions, one MetaField arbitrary, and an
// arbitrary that determines within which stage the meta field will be referenced. Those arbitraries
// are used to generate the pipeline and determine, based on the rules prescribed, if the query
// should succeed or fail on validation.
const firstStageArb = fc.constantFrom(...Object.values(FirstStageOptions));
const metaFieldArb = fc.constantFrom(...Object.values(MetaFields));

// This is not an exhaustive list of stages $meta can be referenced from, but it captures three
// distinct usage patterns. The other possible stages (like $addFields, $setWindowFields) follow the
// same patterns as $project and $group.
const metaReferencingStageNameArb = fc.constantFrom("$project", "$sort", "$group");
let testCaseArb = fc.record({
    firstStage: firstStageArb,
    metaField: metaFieldArb,
    metaReferencingStageName: metaReferencingStageNameArb
});

// Filter out cases where we use a $sort with a meta field that is not a valid sort key.
testCaseArb =
    testCaseArb.filter(({metaField, metaReferencingStageName}) =>
                           (metaReferencingStageName !== "$sort" || metaField.validSortKey));

function generateMetaReferencingStage(stageName, metaFieldName) {
    if (stageName === "$project") {
        return {$project: {a: 0, textField: 0, metaField: {$meta: metaFieldName}}};
    } else if (stageName === "$sort") {
        return {$sort: {a: {$meta: metaFieldName}, _id: 1}};
    } else if (stageName === "$group") {
        return {$group: {_id: "$a", allMetaVals: {$addToSet: {$meta: metaFieldName}}}};
    }
}

function shouldQuerySucceed(
    metaField, firstStage, metaReferencingStageName, hasBlockingStageBeforeMetaReference = false) {
    if (!metaField.shouldBeValidated) {
        return true;
    }

    // There is some inconsistent behavior for $geoNear-related metadata.
    if (metaField === MetaFields.GEO_NEAR_PT || metaField === MetaFields.GEO_NEAR_DIST) {
        // TODO SERVER-100404: If you have a query with a $sort but no $geoNear on a sharded
        // collection, then try to reference $geoNear-related metadata from within a $project or
        // $group (but not $sort), the query will not throw an error as expected.
        if (isSharded && firstStage === FirstStageOptions.SORT &&
            metaReferencingStageName !== "$sort") {
            return true;
        }

        // TODO SERVER-100404: If you have a sharded query with a blocking stage before the
        // meta-referencing stage, validation of $geoNear-related metadata won't occur since the
        // meta-referencing stage won't be sent to the shards. This validation should take place on
        // the router too.
        if (isSharded && hasBlockingStageBeforeMetaReference) {
            return true;
        }

        // TODO SERVER-99965 Mongot queries skip validation for "geoNearDist" and "geoNearPoint".
        if (firstStage === FirstStageOptions.SEARCH ||
            firstStage === FirstStageOptions.SEARCH_W_DETAILS ||
            firstStage === FirstStageOptions.VECTOR_SEARCH ||
            firstStage == FirstStageOptions.RANK_FUSION ||
            firstStage == FirstStageOptions.RANK_FUSION_W_DETAILS) {
            return true;
        }
    }

    // TODO SERVER-100402 If there is a blocking stage before trying to reference "sortKey"
    // metadata, it always succeeds even if there is no sort key.
    if (hasBlockingStageBeforeMetaReference && metaField === MetaFields.SORT_KEY) {
        return true;
    }

    // TODO SERVER-100402: {$meta: "sortKey"} referenced under a $group currently succeeds, even if
    // there is no sort key.
    if (metaReferencingStageName === "$group" && metaField === MetaFields.SORT_KEY) {
        return true;
    }

    return metaField.firstStageRequired.includes(firstStage);
}

// Most cases will fail with kUnavailableMetadataErrCode, but some "sortKey" validation will
// fail with BadValue.
const expectedErrCodes = [kUnavailableMetadataErrCode, ErrorCodes.BadValue];

// First, test just the pipeline with two stages: the first stage may or may not generate metadtata
// fields, and the second stage attempts to reference some metadata field.
fc.assert(fc.property(testCaseArb, ({firstStage, metaField, metaReferencingStageName}) => {
    let metaStage = generateMetaReferencingStage(metaReferencingStageName, metaField.name);

    let pipeline = [firstStage, metaStage];
    if (shouldQuerySucceed(metaField, firstStage, metaReferencingStageName)) {
        assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
    } else {
        assertErrCodeAndErrMsgContains(coll, pipeline, expectedErrCodes, metaField.debugName);
    }
}), {seed: 5, numRuns: 500});

// Also test when we insert a $group stage (which drops all metadata) between the stage that
// generates the metadata and the stage that attempts to reference the metadata.
fc.assert(fc.property(testCaseArb, ({firstStage, metaField, metaReferencingStageName}) => {
    let metaStage = generateMetaReferencingStage(metaReferencingStageName, metaField.name);
    let pipeline = [firstStage, {$group: {_id: null}}, metaStage];

    // TODO SERVER-100443 Since the $group drops per-document metadata, this should always fail for
    // every meta stage that is validated, not just "score" and "scoreDetails".
    const hasBlockingStageBeforeMetaReference = true;
    if (metaField == MetaFields.SCORE || metaField == MetaFields.SCORE_DETAILS ||
        (!shouldQuerySucceed(metaField,
                             firstStage,
                             metaReferencingStageName,
                             hasBlockingStageBeforeMetaReference))) {
        assertErrCodeAndErrMsgContains(coll, pipeline, expectedErrCodes, metaField.debugName);
    } else {
        assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
    }
}), {seed: 5, numRuns: 500});

dropSearchIndex(coll, {name: "search-index"});
dropSearchIndex(coll, {name: "vector-search-index"});
