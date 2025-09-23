/**
 * Tests that extensionOptions are correctly passed and used by the parse_options extension.
 * If 'checkMax' is true and a pipeline contains 'num' value is used that is greater than the
 * options-provided 'max', the pipeline should error.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

function confirmNoMaxRestriction(conn) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    let pipeline = [{$checkNum: {num: Infinity}}];
    assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
}

function confirmMaxValIsEnforced(conn, maxValueToCheck) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    let pipeline = [{$checkNum: {num: maxValueToCheck - 1}}];
    assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));

    pipeline = [{$checkNum: {num: maxValueToCheck + 1}}];
    assertErrorCode(coll, pipeline, 10999106);
}

// Confirm $checkNum is unrestricted when 'checkMax' is false.
withExtensions({"libparse_options_mongo_extension.so": {checkMax: false}}, (conn) => {
    confirmNoMaxRestriction(conn);
});

// Confirm $checkNum is unrestricted when 'checkMax' is false. 'max' is ignored.
withExtensions({"libparse_options_mongo_extension.so": {checkMax: false, max: 1}}, (conn) => {
    confirmNoMaxRestriction(conn);
});

// Confirm $checkNum is restricted when 'checkMax' is true. We expect failure if $checkNum provides a number that is greater than 'max'.
const maxValueToCheck = 100;
withExtensions({"libparse_options_mongo_extension.so": {checkMax: true, max: maxValueToCheck}}, (conn) => {
    confirmMaxValIsEnforced(conn, maxValueToCheck);
});
