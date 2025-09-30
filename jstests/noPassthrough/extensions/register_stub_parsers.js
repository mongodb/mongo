/**
 * Tests that the $stubStage aggregation stage included in aggregation_stage_stub_parsers.json
 * models an unloaded extension by returning the specified error message rather than the generic
 * "unrecognized stage" error.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function runRegisterStubStageTest(conn, fooExtensionLoaded) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    // Test that using $stubStage outputs the expected error.
    let pipeline = [{$stubStage: {}}];
    assertErrorCode(coll, pipeline, 10918500);

    // Test $stubStage in the middle of a complex pipeline.
    pipeline = [{$match: {_id: {$in: [0, 1]}}}, {$stubStage: {}}, {$sort: {x: -1}}];
    assertErrorCode(coll, pipeline, 10918500);

    pipeline = [{$testFoo: {}}];
    if (fooExtensionLoaded) {
        // Test that the provided error message is not shown when the extension is loaded.
        assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
    } else {
        // Test that using $testFoo outputs the expected error when the extension is not loaded.
        assertErrorCode(coll, pipeline, 10918500);
    }
}

// Test without any extensions loaded. We assume all stub stages will error out.
withExtensions({}, (conn) => {
    runRegisterStubStageTest(conn, false);
});

// Test with the foo extension loaded. We assume $testFoo will work, but $stubStage will error out.
withExtensions({"libfoo_mongo_extension.so": {}}, (conn) => {
    runRegisterStubStageTest(conn, true);
});
