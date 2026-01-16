/**
 * Tests that an extension stage can participate in server authorization checks.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

checkPlatformCompatibleWithExtensions();

function runTest(conn) {
    const admin = conn.getDB("admin");
    admin.createUser({user: "adminUser", pwd: "pwd", roles: ["root"]});
    admin.auth({user: "adminUser", pwd: "pwd"});

    const db = conn.getDB(jsTestName());
    const coll = db["bread"];
    assert.commandWorked(coll.insert({_id: 1, breadType: "sourdough"}));

    // First, set up some roles and users to run queries with.

    // This role has all of the privileges required to make toast from the bread collection.
    db.createRole({
        role: "makeToast",
        privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["find", "listIndexes"]}],
        roles: [],
    });
    // This role is allowed to read from the bread collection, but cannot make toast.
    db.createRole({
        role: "noToastingAllowed",
        privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["find"]}],
        roles: [],
    });

    db.createUser({user: "toastMaker", pwd: "pwd", roles: ["makeToast"]});
    db.createUser({user: "cannotToast", pwd: "pwd", roles: ["noToastingAllowed"]});

    admin.logout();

    const toastPipeline = [{$toast: {numSlices: 10, temp: 350.0}}];
    const unionWithToastPipeline = [{$unionWith: {coll: coll.getName(), pipeline: toastPipeline}}];

    {
        // Test a user who is not authorized to make toast.
        db.auth("cannotToast", "pwd");
        // Sanity check to make sure that we can successfully read from the bread collection.
        coll.aggregate().toArray();
        // The command should fail if it includes a $toast stage.
        assertErrorCode(coll, toastPipeline, ErrorCodes.Unauthorized);
        assertErrorCode(coll, unionWithToastPipeline, ErrorCodes.Unauthorized);
        db.logout();
    }

    {
        // Test a user who is authorized to make toast.
        db.auth("toastMaker", "pwd");
        coll.aggregate(toastPipeline).toArray();
        coll.aggregate(unionWithToastPipeline).toArray();
        db.logout();
    }
}

const toasterExtensionOptions = {"libtoaster_mongo_extension.so": {maxToasterHeat: 500.0, allowBagels: true}};

// Standalone and sharded need different configuration setups to enable auth, so we run them separately.

withExtensions(toasterExtensionOptions, runTest, ["standalone"], {}, {auth: ""});

withExtensions(toasterExtensionOptions, runTest, ["sharded"], {other: {keyFile: "jstests/libs/key1"}});
