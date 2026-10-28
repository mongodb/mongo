/**
 * Tests that the extension $listSearchIndexes stage has overridden the built-in implementation when
 * loaded with featureFlagExtensionSearchIndexManagement enabled, and that the built-in fallback
 * implementation is used when the flag is disabled.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const kExtensionLib = "libsearch_index_management_extension.so";
const kEnabledOptions = {setParameter: {featureFlagExtensionSearchIndexManagement: true}};
const kDisabledOptions = {setParameter: {featureFlagExtensionSearchIndexManagement: false}};

/*
 * The extension $listSearchIndexes is a stub that acts as a no-op, passing documents through. The
 * built-in $listSearchIndexes throws SearchNotEnabled when search index management is not
 * configured. We can use this behavior to test whether the extension or the built-in implementation
 * is being used.
 */
const pipeline = [{$listSearchIndexes: {}}];

/*
 * Reads the current value of the featureFlagExtensionSearchIndexManagement flag.
 */
function readFlagValue(adminDb) {
    return assert.commandWorked(
        adminDb.runCommand({getParameter: 1, featureFlagExtensionSearchIndexManagement: 1}),
    ).featureFlagExtensionSearchIndexManagement.value;
}

/*
 * Asserts the implementation selected by the flag currently in effect, and returns that flag value.
 */
function runListSearchIndexesOverrideTest(conn, {shardingTest = null} = {}) {
    const adminDb = conn.getDB("admin");
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();
    const testData = [
        {_id: 0, text: "apple"},
        {_id: 1, text: "banana"},
        {_id: 2, text: "cherry"},
    ];
    assert.commandWorked(coll.insertMany(testData));
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    const featureFlagEnabled = readFlagValue(adminDb);
    if (featureFlagEnabled) {
        // The extension's stub primary parser is selected and passes documents through unchanged.
        assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected: testData});
    } else {
        // The built-in fallback parser is selected, which fails because search index management is
        // not configured on the node running the stage (mongod standalone, or mongos when sharded).
        assert.throwsWithCode(
            () => coll.aggregate(pipeline).toArray(),
            ErrorCodes.SearchNotEnabled,
        );
    }
    return featureFlagEnabled;
}

const observedFlagValues = [];

withExtensions(
    {[kExtensionLib]: {}},
    (conn, shardingTest) =>
        observedFlagValues.push(runListSearchIndexesOverrideTest(conn, {shardingTest})),
    [
        {topology: "standalone", mongodOptions: kEnabledOptions},
        {topology: "standalone", mongodOptions: kDisabledOptions},
        {topology: "sharded", mongodOptions: kEnabledOptions},
        {topology: "sharded", mongodOptions: kDisabledOptions},
    ],
    // Multi-shard so the extension passthrough is exercised across shards.
    {shards: 2, rs: {nodes: 2}},
);

assert.eq(
    observedFlagValues,
    [true, false, true, false],
    "each run should have observed the flag value it was started with",
);
