/**
 * Tests that extensionOptions are correctly passed and used by the test_options extension.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkExtensionFailsToLoad,
    checkPlatformCompatibleWithExtensions,
    deleteExtensionConfigs,
    generateExtensionConfigWithOptions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function testStageRegistration(expectedOptionA, conn) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    const [registered, unregistered] = expectedOptionA ? ["$optionA", "$optionB"] : ["$optionB", "$optionA"];

    {
        const pipeline = [{[registered]: {}}];
        assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
    }

    {
        const pipeline = [{[unregistered]: {}}];
        assertErrorCode(coll, pipeline, 40324, `Unrecognized pipeline stage name: '${unregistered}'`);
    }
}

// Confirm that $optionA is registered and $optionB is not.
withExtensions({"libtest_options_mongo_extension.so": {optionA: true}}, (conn) => {
    testStageRegistration(true, conn);
});

// Confirm that $optionB is registered and $optionA is not.
withExtensions({"libtest_options_mongo_extension.so": {optionA: false}}, (conn) => {
    testStageRegistration(false, conn);
});

// Confirm malformed extensionOptions cause startup to fail.
{
    const st = new ShardingTest({shards: 1, mongos: 1});

    const confirmStartupFailed = (extensionOptions) => {
        const extensionName = generateExtensionConfigWithOptions(
            MongoRunner.getExtensionPath("libtest_options_mongo_extension.so"),
            extensionOptions,
        );
        try {
            checkExtensionFailsToLoad({options: {loadExtensions: extensionName}, st: st});
        } finally {
            deleteExtensionConfigs([extensionName]);
        }
    };

    // Confirm that startup fails if optionA is not a boolean.
    confirmStartupFailed("optionA: 'notABool'");
    // Confirm that startup fails if optionA is missing.
    confirmStartupFailed("notAnOption: true");

    st.stop();
}
