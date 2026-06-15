/**
 * Verifies that extension stages are rejected in $lookup subpipelines when
 * featureFlagExtensionsInsideHybridSearch is disabled.
 *
 * When the flag is off, DocumentSourceExtensionOptimizable::constraints() sets
 * LookupRequirement::kNotAllowed for ALL extensions regardless of the SDK-level allowedInLookup
 * property, so even stages that would be allowed with the flag on (e.g. $testBar) are rejected.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const kNotAllowedInLookupCode = 51047;

withExtensions(
    {"libbar_mongo_extension.so": {}},
    (conn) => {
        const testDB = conn.getDB(jsTestName());
        const localColl = testDB["local"];
        const foreignColl = testDB["foreign"];

        localColl.drop();
        foreignColl.drop();
        assert.commandWorked(localColl.insertOne({_id: 1}));
        assert.commandWorked(foreignColl.insertOne({_id: 10}));

        // $testBar is normally allowed in $lookup when featureFlagExtensionsInsideHybridSearch is
        // on, but with the flag off all extension stages are rejected in $lookup subpipelines.
        assert.commandFailedWithCode(
            testDB.runCommand({
                aggregate: localColl.getName(),
                pipeline: [
                    {
                        $lookup: {
                            from: foreignColl.getName(),
                            pipeline: [{$testBar: {x: 1}}],
                            as: "x",
                        },
                    },
                ],
                cursor: {},
            }),
            kNotAllowedInLookupCode,
            "$testBar should be rejected in $lookup when featureFlagExtensionsInsideHybridSearch is off",
        );

        // Extension stages still work at the top level.
        assert.commandWorked(
            testDB.runCommand({
                aggregate: localColl.getName(),
                pipeline: [{$testBar: {x: 1}}],
                cursor: {},
            }),
        );
    },
    ["standalone"],
    {},
    {setParameter: {featureFlagExtensionsInsideHybridSearch: false}},
);
