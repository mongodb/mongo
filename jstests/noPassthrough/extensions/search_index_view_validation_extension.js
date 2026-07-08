/**
 * Verifies that a search index cannot be created on a view whose definition contains an extension
 * stage that desugars into otherwise-allowed stages ($addFields + $match). Search index CRUD
 * operations validate the raw (un-desugared) view definition, so the validator sees the extension
 * stage ($addFieldsMatch) rather than its desugared form and rejects it.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const invalidStageErrorCode = 10623000;

withExtensionsAndMongot(
    {"libadd_fields_match_mongo_extension.so": {}},
    (conn, mongotMock) => {
        const testDb = conn.getDB(jsTestName());
        const coll = testDb.underlyingSourceCollection;
        coll.drop();
        assert.commandWorked(coll.insertOne({}));

        const viewName = jsTestName();
        assert.commandWorked(
            testDb.createView(viewName, coll.getName(), [
                {
                    $addFieldsMatch: {
                        field: "apple",
                        value: "banana",
                        filter: "grape",
                    },
                },
            ]),
        );

        // The search index validator sees `$addFieldsMatch` (not supported) rather than the
        // desugared form of `$addFields` + `$match` (supported), since desugaring does not happen
        // before CRUD operations on search indexes. The command fails during view validation
        // before contacting mongot, so no mock responses are needed.
        assert.commandFailedWithCode(
            testDb.runCommand({
                createSearchIndexes: viewName,
                indexes: [
                    {
                        name: `${viewName}_index`,
                        definition: {mappings: {dynamic: false, fields: {name: {type: "string"}}}},
                    },
                ],
            }),
            invalidStageErrorCode,
        );
    },
    ["standalone", "sharded"],
);
