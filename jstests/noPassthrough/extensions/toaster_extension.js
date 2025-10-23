/**
 * Tests that extensionOptions are correctly passed and used by the toaster extension. Expects a
 * numeric 'maxToasterHeat' to be supplied for the $toast stage and a boolean 'allowBagels' option
 * to determine whether to register the $toastBagels stage.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

function validateToasterFunctionality(conn, expectBagels) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];

    let pipeline = [{$toast: {temp: 3.5}}];
    assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));

    pipeline = [{$toast: {temp: 7}}];
    assertErrorCode(coll, pipeline, 11285302);

    pipeline = [{$toastBagel: {}}];
    if (expectBagels) {
        assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
    } else {
        assertErrorCode(coll, pipeline, 40324);
    }
}

withExtensions({"libtoaster_mongo_extension.so": {maxToasterHeat: 5, allowBagels: true}}, (conn) => {
    validateToasterFunctionality(conn, true);
});

withExtensions({"libtoaster_mongo_extension.so": {maxToasterHeat: 6, allowBagels: false}}, (conn) => {
    validateToasterFunctionality(conn, false);
});
