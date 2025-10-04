/**
 * Confirm --loadExtensions and $listExtensions fail when 'featureFlagExtensionsAPI' is disabled.
 *
 * TODO SERVER-106932: Remove this test when 'featureFlagExtensionsAPI' is removed.
 */
import {deleteExtensionConfigs, generateExtensionConfigs} from "jstests/noPassthrough/libs/extension_helpers.js";

const extensions = generateExtensionConfigs("libfoo_mongo_extension.so");

try {
    // The 'loadExtensions' startup parameter should fail when featureFlagExtensionsAPI is off.
    {
        try {
            const conn = MongoRunner.runMongod({
                setParameter: {featureFlagExtensionsAPI: false},
                // Use a real extension name to ensure that the feature flag is causing the failure,
                // not a missing extension.
                loadExtensions: extensions[0],
            });
            // If we've reached this point, startup did not fail as expected.
            MongoRunner.stopMongod(conn);
            assert(false, "Expected startup to fail but it succeeded");
        } catch (e) {
            assert.eq(e.returnCode, MongoRunner.EXIT_BADOPTIONS, e);
        }
    }

    // Confirm that $listExtensions fails when featureFlagExtensionsAPI is off.
    {
        const conn = MongoRunner.runMongod({setParameter: {featureFlagExtensionsAPI: false}});
        assert.neq(null, conn, "failed to start mongod");
        const adminDB = conn.getDB("admin");

        // $listExtensions should fail when the feature flag is off.
        const res = adminDB.runCommand({aggregate: 1, pipeline: [{$listExtensions: {}}], cursor: {}});
        assert.commandFailedWithCode(res, ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(conn);
    }
} finally {
    deleteExtensionConfigs(extensions);
}
