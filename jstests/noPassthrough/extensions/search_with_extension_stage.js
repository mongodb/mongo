/**
 * Tests that $search and legacy $vectorSearch can be followed by extension stages.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    mongotCommandForVectorSearchQuery,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

checkPlatformCompatibleWithExtensions();

withExtensionsAndMongot(
    {"libtoaster_mongo_extension.so": {maxToasterHeat: 500.0, allowBagels: true}},
    (conn, mongotMock) => {
        const db = conn.getDB("test");
        const coll = db[jsTestName()];
        coll.drop();
        coll.insert({_id: 0});

        const collUUID = getUUIDFromListCollections(db, coll.getName());
        const mockConn = mongotMock.getConnection();

        // Confirm that $search works when followed by an extension stage.
        {
            mockConn.adminCommand({
                setMockResponses: 1,
                cursorId: NumberLong(1),
                history: [
                    {
                        expectedCommand: {
                            search: coll.getName(),
                            collectionUUID: collUUID,
                            query: {query: "x", path: "y"},
                            $db: "test",
                        },
                        response: {
                            cursor: {
                                id: NumberLong(0),
                                ns: coll.getFullName(),
                                nextBatch: [{_id: 0, $searchScore: 0.9}],
                            },
                            ok: 1,
                        },
                    },
                ],
            });
            assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$search: {query: "x", path: "y"}}, {$loaf: {numSlices: 1}}],
                    cursor: {},
                }),
            );
        }

        // Confirm that $vectorSearch works when followed by an extension stage.
        {
            mongotMock.setMockResponses(
                [
                    {
                        expectedCommand: mongotCommandForVectorSearchQuery({
                            queryVector: [1.0],
                            path: "v",
                            limit: 1,
                            collName: coll.getName(),
                            dbName: "test",
                            collectionUUID: collUUID,
                        }),
                        response: mongotResponseForBatch(
                            [{_id: 0, $vectorSearchScore: 0.9}],
                            NumberLong(0),
                            coll.getFullName(),
                            1,
                        ),
                    },
                ],
                NumberLong(2),
            );
            assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$vectorSearch: {queryVector: [1.0], path: "v", limit: 1}}, {$loaf: {numSlices: 1}}],
                    cursor: {},
                }),
            );
        }
    },
    ["standalone"],
);
