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
 * The $rankFusion feature flag is required to enable use of "score" and "searchScore".
 * @tags: [ featureFlagRankFusionBasic, featureFlagRankFusionFull ]
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
    {_id: 0, a: 3, textField: "three blind mice", geoField: [23, 51]},
    {_id: 1, a: 2, textField: "the three stooges", geoField: [25, 49]},
    {_id: 2, a: 3, textField: "we three kings", geoField: [30, 51]}
]));
assert.commandWorked(coll.createIndex({textField: "text"}));
assert.commandWorked(coll.createIndex({geoField: "2d"}));
assert.commandWorked(
    createSearchIndex(coll, {name: "search-index", definition: {mappings: {dynamic: true}}}));

const kUnavailableMetadataErrCode = 40218;

// The set of stages that may be generated as the initial stage of the pipeline.
const FirstStageOptions = Object.freeze({
    GEO_NEAR: {$geoNear: {near: [25, 25]}},
    FTS_MATCH: {$match: {$text: {$search: "three"}}},
    NON_FTS_MATCH: {$match: {a: {$gt: 3}}},
    SEARCH: {$search: {index: "search-index", exists: {path: "textField"}}},
    SORT: {$sort: {a: -1}}
});

// The set of metadata fields that can be referenced inside $meta, alongside information used to
// dictate if the queries should succeed or not.
// TODO SERVER-93521: Change "searchScore" and "vectorSearchScore" to be validated.
// TODO SERVER-99169: Change "score" and "scoreDetails" to be validated.
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
        shouldBeValidated: false,
        debugName: "$search score",
        validSortKey: true
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
        firstStageRequired: [FirstStageOptions.SORT, FirstStageOptions.GEO_NEAR]
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
        shouldBeValidated: false,
        debugName: "$vectorSearch distance",
        validSortKey: true
    },
    SCORE: {name: "score", shouldBeValidated: false, debugName: "score", validSortKey: true},
    SCORE_DETAILS: {
        name: "scoreDetails",
        shouldBeValidated: false,
        debugName: "scoreDetails",
        validSortKey: false
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

function shouldQuerySucceed(metaField, firstStage, metaReferencingStageName) {
    if (!metaField.shouldBeValidated) {
        return true;
    }

    // TODO SERVER-100404: There is some inconsistent behavior on sharded collections, identified
    // here. In summary, if you have a query with a $sort but no $geoNear on a sharded collection,
    // then try to reference $geoNear-related metadata from within a $project or $group (but not
    // $sort), the query will not throw an error as expected.
    if (isSharded &&
        ((metaField === MetaFields.GEO_NEAR_PT || metaField === MetaFields.GEO_NEAR_DIST) &&
         firstStage === FirstStageOptions.SORT && metaReferencingStageName !== "$sort")) {
        return true;
    }

    // TODO SERVER-100394: We should make meta field validation take place when $search is the first
    // stage, but for now it's skipped.
    if (firstStage === FirstStageOptions.SEARCH) {
        // This is a very specific edge case that's inconsistent but actually is the proper
        // behavior. Even though validation is typically skipped on $search queries for now, a
        // sharded $search query will perform validation for references to "textScore". Once
        // SERVER-100394 is done, all $search cases should follow suit.
        if (isSharded && metaField === MetaFields.TEXT_SCORE) {
            return false;
        }

        return true;
    }

    // TODO SERVER-100402: {$meta: "sortKey"} referenced under a $group currently succeeds, even if
    // there is no sort key.
    if (metaReferencingStageName === "$group" && metaField === MetaFields.SORT_KEY) {
        return true;
    }

    return metaField.firstStageRequired.includes(firstStage);
}

fc.assert(fc.property(testCaseArb, ({firstStage, metaField, metaReferencingStageName}) => {
    let metaStage = generateMetaReferencingStage(metaReferencingStageName, metaField.name);
    let pipeline = [firstStage, metaStage];

    if (shouldQuerySucceed(metaField, firstStage, metaReferencingStageName)) {
        assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
    } else {
        // Most cases will fail with kUnavailableMetadataErrCode, but some "sortKey" validation will
        // fail with BadValue.
        const expectedErrCodes = [kUnavailableMetadataErrCode, ErrorCodes.BadValue];
        assertErrCodeAndErrMsgContains(coll, pipeline, expectedErrCodes, metaField.debugName);
    }
}), {seed: 5, numRuns: 500});

dropSearchIndex(coll, {name: "search-index"});
