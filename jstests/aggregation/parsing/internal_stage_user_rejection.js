/**
 * Audits that all internal-only aggregation stages are correctly rejected when submitted by
 * external (user-facing) clients. This test checks that every stage enumerated by $listMqlEntities
 * must be explicitly classified here, ensuring new internal stages cannot be added without proper
 * rejection gating being verified.
 *
 * Each stage falls into exactly one of five categories based on its AllowedWithApiStrict and
 * AllowedWithClientType registration flags:
 *
 *   kInternalClientTypeStages — registered with REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE,
 *     which sets both AllowedWithApiStrict::kInternal and AllowedWithClientType::kInternal.
 *     External clients are unconditionally rejected via assertAllowedInternalIfRequired.
 *
 *   kApiStrictInternalStages — AllowedWithApiStrict::kInternal + AllowedWithClientType::kAny.
 *     External clients are only rejected when apiStrict:true is set, via
 *     assertLanguageFeatureIsAllowed.
 *
 *   kNeverInVersion1Stages — AllowedWithApiStrict::kNeverInVersion1.
 *     Rejected when apiStrict:true is set in API version 1, via assertLanguageFeatureIsAllowed.
 *
 *   kAlwaysAllowedStages — AllowedWithApiStrict::kAlways or kConditionally.
 *     Available to all external clients.
 *
 *   kTestOnlyStages — REGISTER_TEST_LITE_PARSED_DOCUMENT_SOURCE (kNeverInVersion1 + test-only).
 *     Only active when enableTestCommands is set.
 *
 * @tags: [
 *   uses_api_parameters,
 *   featureFlagExtensionsAPI,            # $listExtensions
 *   assumes_read_concern_unchanged,      # $currentOp rejects non-local readConcern before the API strict check
 *   do_not_wrap_aggregations_in_facets,  # $listMqlEntities is not allowed inside $facet
 *   known_query_shape_computation_problem,  # Internal stages sent with malformed/empty specs
 *                                           # cannot be safely full-parsed when the query-shape
 *                                           # replay hook ($queryStats) reprocesses them.
 *   incompatible_with_extensions,        # Extension variants register stages this test's static
 *                                        # classification does not enumerate.
 * ]
 */
// $listLocalSessions targets a specific mongos and isn't supported with random mongos dispatching.
TestData.pinToSingleMongos = true;

import {describe, it, before} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const collName = "testColl";

// assertAllowedInternalIfRequired fires this location code (unconditional client-type rejection).
const kInternalClientRejectionCode = 5491300;
// assertLanguageFeatureIsAllowed fires this when apiStrict:true blocks a stage. Some stages whose
// lite-parsers run IDL argument validation before the API check reach IDLFailedToParse instead;
// both codes represent a valid external-client rejection for kApiStrictInternalStages.
const kApiStrictRejectionCode = [ErrorCodes.APIStrictError, ErrorCodes.IDLFailedToParse];

testDB.dropDatabase();

// Some kNeverInVersion1Stages run a topology/config precondition check before the API strict
// check fires, so on certain fixtures they fail with a different code. Either code still proves
// the stage is inaccessible to external clients, so both are accepted for that stage.
const kTopologyPrecheckCodes = {
    // Not supported on a standalone mongod.
    "$listSampledQueries": [ErrorCodes.IllegalOperation],
    "$_analyzeShardKeyReadWriteDistribution": [ErrorCodes.IllegalOperation],
    // Can only be run on a router (mongos), not mongod (6789101); and must be run against the
    // admin database (6789102), which the sub-pipeline tests don't do since they nest the stage
    // inside an aggregate against testDB.
    "$shardedDataDistribution": [6789101, 6789102],
};

// Stages whose lite-parser IDL-validates the (empty) spec before the gating check, so they
// surface IDLFailedToParse instead of the gating code. Accepted per-stage rather than globally.
const kEmptySpecParseFailureCodes = {
    "$_internalDocumentResultsAndMetadata": [ErrorCodes.IDLFailedToParse],
};

function acceptedCodesFor(stageName, expectedCode) {
    const base = Array.isArray(expectedCode) ? expectedCode : [expectedCode];
    return base
        .concat(kTopologyPrecheckCodes[stageName] || [])
        .concat(kEmptySpecParseFailureCodes[stageName] || []);
}

// ---------------------------------------------------------------------------
// Stage lists — each stage must appear in exactly one list.
// ---------------------------------------------------------------------------

/**
 * Registered with REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE:
 *   AllowedWithClientType::kInternal + AllowedWithApiStrict::kInternal
 *
 * External clients are ALWAYS rejected regardless of API parameters.
 */
const kInternalClientTypeStages = new Set([
    // Change stream internal stages
    "$_internalChangeStreamAddPostImage",
    "$_internalChangeStreamAddPreImage",
    "$_internalChangeStreamCheckInvalidate",
    "$_internalChangeStreamCheckResumability",
    "$_internalChangeStreamCheckTopologyChange",
    "$_internalChangeStreamHandleTopologyChange",
    "$_internalChangeStreamInjectControlEvents",
    "$_internalChangeStreamOplogMatch",
    "$_internalChangeStreamTransform",
    "$_internalChangeStreamUnwindTransaction",
    // Densify
    "$_internalDensify",
    // Find-and-modify oplog image lookup
    "$_internalFindAndModifyImageLookup",
    // Search
    "$_internalDocumentResultsAndMetadata",
    "$_internalHybridSearch",
    "$_internalSearchIdLookup",
    "$_internalStreamTerminator",
    // Resharding
    "$_addReshardingResumeId",
    "$_internalReshardingIterateTransaction",
    "$_internalReshardingOwnershipMatch",
    // Router-side merge
    "$mergeCursors",
    // Pipeline execution utilities
    "$queue",
    "$setMetadata",
    "$setVariableFromSubPipeline",
    // Enterprise Atlas Streams stages (may be absent in community builds)
    "$cachedLookup",
    "$externalFunction",
    "$https",
    "$setStreamMeta",
    "$_streamsVectorSearch",
    "$validate",
    "$tumblingWindow",
    "$hoppingWindow",
    "$sessionWindow",
]);

/**
 * AllowedWithApiStrict::kInternal + AllowedWithClientType::kAny
 *
 * External clients are rejected when apiStrict:true is set. Without apiStrict they pass the
 * lite-parse client-type check because these stages use AllowedWithClientType::kAny rather than
 * kInternal.
 */
const kApiStrictInternalStages = new Set([
    "$_internalAllCollectionStats",
    "$_internalComputeGeoNearDistance",
    "$_internalConvertBucketIndexStats",
    "$_internalListCollections",
    // Enterprise hot-backup (absent in community builds)
    "$_backupFile",
]);

/**
 * AllowedWithApiStrict::kNeverInVersion1
 *
 * Rejected with (apiVersion:"1", apiStrict:true) but allowed for external clients otherwise.
 */
const kNeverInVersion1Stages = new Set([
    // Timeseries-bounded sort (in test mode: kNeverInVersion1+kAny; in production: kInternal+kInternal
    // — either way rejected by apiStrict:true)
    "$_internalBoundedSort",
    // Change stream utilities
    "$changeStreamSplitLargeEvent",
    // Admin / diagnostic stages
    "$currentOp",
    "$fill",
    "$indexStats",
    "$listCatalog",
    "$listClusterCatalog",
    "$listExtensions",
    "$listLocalSessions",
    "$listSampledQueries",
    "$listSessions",
    "$planCacheStats",
    "$querySettings",
    // $querySettings desugar internals
    "$_internalListQuerySettings",
    "$_internalQuerySettingsDebugShape",
    "$queryStats",
    "$shardedDataDistribution",
    // Scoring / ranking stages
    "$rankFusion",
    "$score",
    "$scoreFusion",
    // Search (requires Atlas Search; present as stubs otherwise)
    "$search",
    "$searchBeta",
    "$searchMeta",
    "$vectorSearch",
    "$listSearchIndexes",
    // Shard-key analysis
    "$_analyzeShardKeyReadWriteDistribution",
    // Oplog / internal routing utilities
    "$_internalApplyOplogUpdate",
    "$_internalInhibitOptimization",
    "$_internalJoinHint",
    "$_internalShardServerInfo",
    "$_internalShredDocuments",
    "$_internalSplitPipeline",
]);

/**
 * AllowedWithApiStrict::kAlways or kConditionally
 *
 * Available to all external clients included here solely to satisfy the coverage check.
 */
const kAlwaysAllowedStages = new Set([
    "$addFields",
    "$bucket",
    "$bucketAuto",
    "$changeStream",
    "$collStats",
    "$count",
    "$densify",
    "$documents",
    "$facet",
    "$geoNear",
    "$graphLookup",
    "$group",
    "$limit",
    "$lookup",
    "$match",
    "$merge",
    "$out",
    "$project",
    "$redact",
    "$replaceRoot",
    "$replaceWith",
    "$sample",
    "$set",
    "$setWindowFields",
    "$skip",
    "$sort",
    "$sortByCount",
    "$unionWith",
    "$unset",
    "$unwind",
    // Timeseries bucket internals — allowed externally for transparent query routing
    "$_internalUnpackBucket",
    "$_unpackBucket",
    // Window / streaming group internals surfaced via setWindowFields
    "$_internalSetWindowFields",
    "$_internalStreamingGroup",
    // Enterprise hot-backup cursors (kAlways; absent in community builds)
    "$backupCursor",
    "$backupCursorExtend",
]);

/**
 * REGISTER_TEST_LITE_PARSED_DOCUMENT_SOURCE: only active under enableTestCommands. Excluded from
 * rejection checks.
 */
const kTestOnlyStages = new Set([
    "$_internalAssertDataAssumptions",
    "$listCachedAndActiveUsers",
    "$listMqlEntities",
    "$listQueryKnobs",
]);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Returns the set of all stage names currently registered via $listMqlEntities.
 */
function fetchAllStageNames() {
    const names = db
        .getSiblingDB("admin")
        .aggregate([{$listMqlEntities: {entityType: "aggregationStages"}}])
        .toArray()
        .map((s) => s.name);
    return new Set(names);
}

/**
 * Runs a single-stage pipeline as an external client with the supplied API parameters and
 * returns the raw command result. Uses an empty stage spec ({}) because the API-level rejection
 * fires in the lite-parsed phase before argument validation.
 */
function runStageAsExternalClient(stageName, apiParams) {
    return testDB.runCommand(
        Object.assign({aggregate: collName, pipeline: [{[stageName]: {}}], cursor: {}}, apiParams),
    );
}

/**
 * Asserts that every stage in `stageSet` that is present in `allStages` is rejected with
 * `expectedCode` when run with `apiParams`.
 */
function assertAllRejected(stageSet, apiParams, allStages, expectedCode) {
    for (const stageName of stageSet) {
        if (!allStages.has(stageName)) continue;
        assert.commandFailedWithCode(
            runStageAsExternalClient(stageName, apiParams),
            acceptedCodesFor(stageName, expectedCode),
            {message: `${stageName} must be rejected for external clients`, apiParams},
        );
    }
}

/**
 * Asserts that every available stage in `stageSet` is rejected with `expectedCode` when nested
 * inside $facet, $lookup, and $unionWith with `apiParams`. `expectedCode` may be a single code
 * or an array.
 */
function assertRejectedInSubPipelines(stageSet, apiParams, allStages, expectedCode) {
    for (const stageName of stageSet) {
        if (!allStages.has(stageName)) continue;
        for (const [wrapper, pipeline] of [
            ["$facet", [{$facet: {branch: [{[stageName]: {}}]}}]],
            ["$lookup", [{$lookup: {from: collName, pipeline: [{[stageName]: {}}], as: "r"}}]],
            ["$unionWith", [{$unionWith: {coll: collName, pipeline: [{[stageName]: {}}]}}]],
        ]) {
            assert.commandFailedWithCode(
                testDB.runCommand(
                    Object.assign({aggregate: collName, pipeline, cursor: {}}, apiParams),
                ),
                acceptedCodesFor(stageName, expectedCode),
                {message: `${stageName} must be rejected when nested inside ${wrapper}`, apiParams},
            );
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe("Internal aggregation stage user rejection audit", function () {
    let allStages;

    before(function () {
        allStages = fetchAllStageNames();

        // Fail fast, before any per-category assertions run, if a stage isn't classified into
        // exactly one of the lists above. A "before all" hook failure skips every it() below.
        const allSets = [
            kInternalClientTypeStages,
            kApiStrictInternalStages,
            kNeverInVersion1Stages,
            kAlwaysAllowedStages,
            kTestOnlyStages,
        ];
        const allKnown = new Set(allSets.flatMap((s) => [...s]));

        // Every stage listed in this test file must belong to exactly one set.
        const totalAcrossSets = allSets.reduce((sum, s) => sum + s.size, 0);
        assert.eq(
            totalAcrossSets,
            allKnown.size,
            "A stage appears in more than one classification list. Each stage must belong to exactly one.",
        );

        const uncovered = [...allStages].filter((s) => !allKnown.has(s));
        assert(
            uncovered.length === 0,
            "Uncovered stages found in $listMqlEntities. Add each to the appropriate list in this test file.",
            {uncovered},
        );
    });

    it("kInternalClientTypeStages: rejected even without API parameters", function () {
        assertAllRejected(kInternalClientTypeStages, {}, allStages, kInternalClientRejectionCode);
    });

    it("kApiStrictInternalStages: rejected with apiStrict:true", function () {
        assertAllRejected(
            kApiStrictInternalStages,
            {apiVersion: "1", apiStrict: true},
            allStages,
            kApiStrictRejectionCode,
        );
    });

    it("kApiStrictInternalStages: not rejected by client-type gate without API parameters", function () {
        for (const stageName of kApiStrictInternalStages) {
            if (!allStages.has(stageName)) {
                continue;
            }
            const result = runStageAsExternalClient(stageName, {});
            assert.neq(
                result.code,
                kInternalClientRejectionCode,
                `${stageName} should not be rejected by the internal client-type gate without apiStrict. ` +
                    `If it now is, move it to kInternalClientTypeStages`,
            );
        }
    });

    it("kInternalClientTypeStages: rejected when embedded in sub-pipelines", function () {
        assertRejectedInSubPipelines(
            kInternalClientTypeStages,
            {},
            allStages,
            kInternalClientRejectionCode,
        );
    });

    it("kApiStrictInternalStages: rejected in sub-pipelines with apiStrict:true", function () {
        assertRejectedInSubPipelines(
            kApiStrictInternalStages,
            {apiVersion: "1", apiStrict: true},
            allStages,
            kApiStrictRejectionCode,
        );
    });

    it("kNeverInVersion1Stages: rejected in sub-pipelines with apiStrict:true", function () {
        assertRejectedInSubPipelines(
            kNeverInVersion1Stages,
            {apiVersion: "1", apiStrict: true},
            allStages,
            kApiStrictRejectionCode,
        );
    });

    it("kNeverInVersion1Stages: rejected with apiStrict:true in API version 1", function () {
        assertAllRejected(
            kNeverInVersion1Stages,
            {apiVersion: "1", apiStrict: true},
            allStages,
            kApiStrictRejectionCode,
        );
    });
});
