/**
 * Utility functions for upgrade_enables_extension_foo.js,
 * upgrade_enables_extension_foo_auth.js, extension_foo_upgrade_downgrade.js, and
 * extension_foo_upgrade_downgrade_auth.js.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {generateExtensionConfigs, deleteExtensionConfigs} from "jstests/noPassthrough/libs/extension_helpers.js";

const fooStageUnrecognizedErrCode = 40324;
const fooParseErrorCodes = [11165101];

const collName = jsTestName();
const viewName = "foo_view";
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

const docs = [
    {_id: 0, foo: "dreaming"},
    {_id: 1, foo: "about"},
    {_id: 2, foo: "super cool extensions"},
];

/**
 * Generate extension configs for upgrade_enables_extension_foo tests.
 */
export function generateUpgradeEnablesExtensionConfigs() {
    return generateExtensionConfigs(["libfoo_mongo_extension.so", "libvector_search_extension.so"]);
}

/**
 * Generate extension configs for extension_foo_upgrade_downgrade tests.
 */
export function generateExtensionUpgradeDowngradeConfigs() {
    return generateExtensionConfigs(["libfoo_mongo_extension.so", "libfoo_extension_v2.so"]);
}

export function deleteMultiversionExtensionConfigs(extensionNames) {
    deleteExtensionConfigs(extensionNames);
}

export function extensionNodeOptions(extensionName) {
    return {
        loadExtensions: [extensionName],
        setParameter: {featureFlagExtensionsAPI: true},
    };
}

export function multipleExtensionNodeOptions(extensionNames) {
    return {
        loadExtensions: extensionNames,
        setParameter: {featureFlagExtensionsAPI: true},
    };
}

export function wrapOptionsWithStubParserFeatureFlag(baseOptions) {
    return {
        ...baseOptions,
        setParameter: {
            ...baseOptions.setParameter,
            featureFlagExtensionStubParsers: true,
        },
    };
}

export function wrapOptionsWithViewsAndUnionWithFeatureFlag(baseOptions) {
    return {
        ...baseOptions,
        setParameter: {
            ...baseOptions.setParameter,
            featureFlagExtensionViewsAndUnionWith: true,
        },
    };
}

/**
 * Setup collection with test data.
 */
export function setupCollection(primaryConn, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConn), collName);

    if (shardingTest) {
        // Enable sharding on this collection by _id. After inserting the data below,
        // this will distribute 2 documents to one shard and 1 document to the other.
        shardingTest.shardColl(coll, {_id: 1});
    }

    assert.commandWorked(coll.insertMany(docs));
}

function assertFooStageAccepted(primaryConn) {
    const db = getDB(primaryConn);

    // $testFoo is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo is accepted in a view pipeline.
    assertFooViewCreationAllowed(primaryConn);
}

function assertFooViewCreationRejected(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();
    assert.commandFailedWithCode(db.createView(viewName, collName, [{$testFoo: {}}]), fooStageUnrecognizedErrCode);
}

function assertFooViewCreationAllowed(primaryConn, queryable = true) {
    const db = getDB(primaryConn);
    db[viewName].drop();
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    if (!queryable) {
        return;
    }

    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});
}

/*
 * Extension $vectorSearch expects an empty spec and acts as a no-op. Legacy $vectorSearch throws
 * SearchNotEnabled when mongot is not configured. We can use this behavior to test whether
 * extension or legacy vector search is being used.
 */
const vectorSearchPipeline = [{$vectorSearch: {}}];
const vectorSearchInUnionWithPipeline = [{$unionWith: {coll: collName, pipeline: [{$vectorSearch: {}}]}}];

export function assertLegacyVectorSearchUsed(primaryConn) {
    const db = getDB(primaryConn);
    const coll = db[collName];
    assert.throwsWithCode(() => coll.aggregate(vectorSearchPipeline), ErrorCodes.SearchNotEnabled);
}

export function assertExtensionVectorSearchUsed(primaryConn) {
    const db = getDB(primaryConn);
    const coll = db[collName];
    assertArrayEq({actual: coll.aggregate(vectorSearchPipeline).toArray(), expected: docs});
}

export function assertLegacyVectorSearchInUnionWithUsed(primaryConn) {
    const db = getDB(primaryConn);
    const coll = db[collName];
    assert.throwsWithCode(() => coll.aggregate(vectorSearchInUnionWithPipeline), ErrorCodes.SearchNotEnabled);
}

export function assertExtensionVectorSearchInUnionWithUsed(primaryConn) {
    const db = getDB(primaryConn);
    const coll = db[collName];
    // $unionWith returns docs from both the main collection and the unionWith subpipeline.
    // Since both are the same collection and $vectorSearch acts as a no-op, we get all docs twice.
    const expectedDocs = [...docs, ...docs];
    assertArrayEq({actual: coll.aggregate(vectorSearchInUnionWithPipeline).toArray(), expected: expectedDocs});
}

export function assertVectorSearchInUnionWithBasedOnFeatureFlag(primaryConn) {
    if (FeatureFlagUtil.isPresentAndEnabled(primaryConn, "ExtensionViewsAndUnionWith")) {
        assertExtensionVectorSearchInUnionWithUsed(primaryConn);
    } else {
        assertLegacyVectorSearchInUnionWithUsed(primaryConn);
    }
}

export function assertFooViewCreationRejectedAndLegacyVectorSearchUsed(primaryConn) {
    assertFooViewCreationRejected(primaryConn);
    assertLegacyVectorSearchUsed(primaryConn);
    assertLegacyVectorSearchInUnionWithUsed(primaryConn);
}

export function assertFooViewAllowedAndLegacyVectorSearchUsed(primaryConn) {
    assertFooViewCreationAllowed(primaryConn);
    assertLegacyVectorSearchUsed(primaryConn);
    assertLegacyVectorSearchInUnionWithUsed(primaryConn);
}

export function assertFooViewAndExtensionVectorSearchUsed(primaryConn) {
    assertFooStageAccepted(primaryConn);
    assertExtensionVectorSearchUsed(primaryConn);
    assertVectorSearchInUnionWithBasedOnFeatureFlag(primaryConn);
}

export function assertFooViewCreationOnlyAllowedAndLegacyVectorSearchUsed(primaryConn) {
    // During the mixed-version upgrade process, view creation with $testFoo may succeed or fail
    // depending on whether mongos validates the pipeline first. This behavior is flaky because
    // mongos sometimes forwards the create command to shards without validation (succeeds), and
    // other times validates locally first (fails).
    const db = getDB(primaryConn);
    db[viewName].drop();
    const createViewResult = db.createView(viewName, collName, [{$testFoo: {}}]);
    // Either outcome is acceptable in this mixed-version state.
    assert(
        createViewResult["ok"] === 1 || createViewResult.code === fooStageUnrecognizedErrCode,
        "Expected view creation to either succeed or fail with unrecognized stage error, got: " +
            tojson(createViewResult),
    );
    assertLegacyVectorSearchUsed(primaryConn);
    assertLegacyVectorSearchInUnionWithUsed(primaryConn);
}

export function assertFooStageAcceptedV1Only(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo in v1 should reject a non-empty stage definition.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}}),
        fooParseErrorCodes,
    );

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});
}

// TODO SERVER-115501 Remove this helper.
export function assertFooStageAcceptedEitherVersion(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    const response = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert(response.ok === 1 || response.code in fooParseErrorCodes);
}

export function assertFooStageAcceptedV1AndV2(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    let result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo V2 now accepts a non-empty stage definition.
    result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    let viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});

    db[viewName].drop();

    // $testFoo with a non-empty stage definition is now accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {nonEmpty: true}}]));
    viewResult = assert.commandWorked(db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}));
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});
}

export function assertFooStageAcceptedV1OnlyPlusV2ViewCreation(primaryConn) {
    const db = getDB(primaryConn);
    db[viewName].drop();

    // $testFoo should reject a non-empty stage definition in an aggregation command.
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collName, pipeline: [{$testFoo: {nonEmpty: true}}], cursor: {}}),
        fooParseErrorCodes,
    );

    // $testFoo with empty stage definition is accepted in a plain aggregation command.
    const result = db.runCommand({aggregate: collName, pipeline: [{$testFoo: {}}], cursor: {}});
    assert.commandWorked(result);
    assertArrayEq({actual: result.cursor.firstBatch, expected: docs});

    // $testFoo with empty stage definition is accepted in a view pipeline.
    assert.commandWorked(db.createView(viewName, collName, [{$testFoo: {}}]));
    const viewResult = assert.commandWorked(
        db.runCommand({aggregate: viewName, pipeline: [{$match: {_id: 0}}], cursor: {}}),
    );
    assertArrayEq({actual: viewResult.cursor.firstBatch, expected: [docs[0]]});

    // This behavior is flaky. Most of the time here, $testFoo with a non-empty stage definition is
    // allowed in a view pipeline, but queries over that view cannot yet be run. Occasionally,
    // however, the createView() command is rejected first for not recognizing $testFoo V2 yet.
    db[viewName].drop();
    const createViewResult = db.createView(viewName, collName, [{$testFoo: {nonEmpty: true}}]);
    if (createViewResult["ok"]) {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: viewName, pipeline: [], cursor: {}}),
            fooParseErrorCodes,
        );
    } else {
        assert.commandFailedWithCode(createViewResult, fooParseErrorCodes);
    }
}

export function assertFooViewCreationAllowedAndLegacyVectorSearchUsed(primaryConn) {
    assertFooViewCreationAllowed(primaryConn);
    assertLegacyVectorSearchUsed(primaryConn);
    assertLegacyVectorSearchInUnionWithUsed(primaryConn);
}

export function assertFooViewCreationAllowedAndExtensionVectorSearchUsed(primaryConn) {
    assertFooStageAccepted(primaryConn);
    assertExtensionVectorSearchUsed(primaryConn);
    assertVectorSearchInUnionWithBasedOnFeatureFlag(primaryConn);
}

export function assertFooViewCreationAndVectorSearchBehaviorAfterPrimaryUpgrade(primaryConn) {
    const vectorSearchExtensionFlagValue = primaryConn.getDB("admin").runCommand({
        getParameter: 1,
        featureFlagVectorSearchExtension: 1,
    }).featureFlagVectorSearchExtension;
    if (vectorSearchExtensionFlagValue && vectorSearchExtensionFlagValue.currentlyEnabled) {
        assertFooViewAndExtensionVectorSearchUsed(primaryConn);
    } else {
        assertFooViewAllowedAndLegacyVectorSearchUsed(primaryConn);
    }
}

/**
 * Asserts that when only the router (mongos) has the IFR flag enabled, we get correct extension
 * $vectorSearch behavior. The router propagates the flag to shards, so extension behavior is used.
 * Note: The test fixture sets the flags before calling this function.
 */
export function assertOnlyRouterHasIFRFlagAndExtensionVectorSearchUsed(primaryConn, shardingTest) {
    // Router propagates flag to shards, so extension behavior should be used
    assertExtensionVectorSearchUsed(primaryConn);
    assertVectorSearchInUnionWithBasedOnFeatureFlag(primaryConn);
}

/**
 * Asserts that when only a shard has the IFR flag enabled (router doesn't), we get correct legacy
 * $vectorSearch behavior. Since the router doesn't have the flag, it won't propagate it, so legacy
 * behavior is used.
 * Note: The test fixture sets the flags before calling this function.
 */
export function assertOnlyShardHasIFRFlagAndLegacyVectorSearchUsed(primaryConn, shardingTest) {
    // Router doesn't have flag, so it won't propagate it - legacy behavior should be used
    assertLegacyVectorSearchUsed(primaryConn);
    assertLegacyVectorSearchInUnionWithUsed(primaryConn);
}

/**
 * Asserts that when all nodes (router and shards) have the IFR flag enabled, we get correct
 * extension $vectorSearch behavior.
 * Note: The test fixture sets the flags before calling this function.
 */
export function assertAllNodesHaveIFRFlagAndExtensionVectorSearchUsed(primaryConn, shardingTest) {
    // All nodes have flag, so extension behavior should be used
    assertExtensionVectorSearchUsed(primaryConn);
    assertVectorSearchInUnionWithBasedOnFeatureFlag(primaryConn);
}
