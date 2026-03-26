/**
 * Tests that the featureFlagSearchExtension IFR flag correctly toggles between the extension
 * $search implementation (primary) and the legacy $search implementation (fallback) at runtime.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

/*
 * Extension $search accepts an empty spec and acts as a no-op, returning all documents.
 * Legacy $search throws SearchNotEnabled when mongot is not configured. We can use this
 * behavior to test whether extension or legacy $search is being used.
 */
const pipeline = [{$search: {}}];

/*
 * Test with no extensions loaded. Legacy (fallback) should always be used, regardless of feature
 * flag state.
 */
withExtensions({}, (conn) => {
    const adminDb = conn.getDB("admin");
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertOne({_id: 0}));

    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: true}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);

    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: false}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
});

/*
 * Test with search extension loaded. Flag toggles between extension and legacy implementations.
 */
withExtensions({"libsearch_extension.so": {}}, (conn) => {
    const adminDb = conn.getDB("admin");
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();

    const testData = [
        {_id: 0, text: "apple"},
        {_id: 1, text: "banana"},
        {_id: 2, text: "cherry"},
    ];
    assert.commandWorked(coll.insertMany(testData));

    // Flag enabled; extension is used (no-op, returns all documents).
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: true}));
    assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected: testData});

    // Flag disabled; legacy is used (errors because mongot is not configured).
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: false}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Toggles correctly with a more complex pipeline.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: true}));
    const complexPipeline = [{$search: {}}, {$match: {_id: {$in: [0, 2]}}}, {$project: {text: 1, _id: 0}}];
    assertArrayEq({
        actual: coll.aggregate(complexPipeline).toArray(),
        expected: [{text: "apple"}, {text: "cherry"}],
    });

    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: false}));
    assert.throwsWithCode(() => coll.aggregate(complexPipeline).toArray(), ErrorCodes.SearchNotEnabled);
});
