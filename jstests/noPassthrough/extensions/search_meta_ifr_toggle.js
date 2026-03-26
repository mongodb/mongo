/**
 * Tests that the featureFlagSearchExtension IFR flag correctly toggles between the extension
 * $searchMeta implementation (primary) and the legacy $searchMeta implementation (fallback) at
 * runtime.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

/*
 * Extension $searchMeta accepts an empty spec and acts as a no-op, returning one metadata
 * document. Legacy $searchMeta throws SearchNotEnabled when mongot is not configured. We can
 * use this behavior to test whether extension or legacy $searchMeta is being used.
 */
const pipeline = [{$searchMeta: {}}];

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
    assert.commandWorked(
        coll.insertMany([
            {_id: 0, text: "apple"},
            {_id: 1, text: "banana"},
            {_id: 2, text: "cherry"},
        ]),
    );

    // Flag enabled; extension is used (no-op, returns all documents).
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: true}));
    assertArrayEq({
        actual: coll.aggregate(pipeline).toArray(),
        expected: [
            {_id: 0, text: "apple"},
            {_id: 1, text: "banana"},
            {_id: 2, text: "cherry"},
        ],
    });

    // Flag disabled; legacy is used (errors because mongot is not configured).
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: false}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Toggles back to extension correctly.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: true}));
    assertArrayEq({
        actual: coll.aggregate(pipeline).toArray(),
        expected: [
            {_id: 0, text: "apple"},
            {_id: 1, text: "banana"},
            {_id: 2, text: "cherry"},
        ],
    });

    // Toggles back to legacy correctly.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagSearchExtension: false}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
});
