/**
 * Verifies DocsNeededBounds propagation end-to-end for extension stages by observing the batchSize mongod
 * sends mongot on the initial $search request.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {mongotCommandForQuery, mongotResponseForBatch} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

checkPlatformCompatibleWithExtensions();

// Mirrors kDefaultMongotBatchSize in src/mongo/db/query/search/mongot_cursor.h.
const kDefaultMongotBatchSize = 101;

withExtensionsAndMongot(
    {
        "libfoo_mongo_extension.so": {},
        "liblimit_mongo_extension.so": {},
    },
    (conn, mongotMock) => {
        const dbName = "test";
        const db = conn.getDB(dbName);

        if (checkSbeRestrictedOrFullyEnabled(db) && FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "SearchInSbe")) {
            jsTest.log.info("Skipping: $search in SBE uses a different cursor-establishment path.");
            return;
        }

        // Pin oversubscriptionFactor to 1.0 so the expected batchSize is deterministic and directly
        // reflects the bounds we compute.
        assert.commandWorked(
            db.adminCommand({setClusterParameter: {internalSearchOptions: {oversubscriptionFactor: 1}}}),
        );

        const coll = db[jsTestName()];
        coll.drop();
        for (let i = 0; i < 20; i++) {
            assert.commandWorked(coll.insert({_id: i, x: i}));
        }
        const collUUID = getUUIDFromListCollections(db, coll.getName());
        const mockConn = mongotMock.getConnection();
        const searchQuery = {query: "x", path: "y"};

        // Prime mongot to expect a $search with cursorOptions.batchSize === expectedBatchSize. If
        // the server sends a different batchSize, mongotmock fails the expectedCommand match and
        // the aggregate errors out.
        let nextCursorId = 1;
        function primeMongot(expectedBatchSize) {
            mockConn.adminCommand({
                setMockResponses: 1,
                cursorId: NumberLong(nextCursorId++),
                history: [
                    {
                        expectedCommand: mongotCommandForQuery({
                            query: searchQuery,
                            collName: coll.getName(),
                            db: dbName,
                            collectionUUID: collUUID,
                            cursorOptions: {batchSize: NumberLong(expectedBatchSize)},
                        }),
                        response: mongotResponseForBatch(
                            [
                                {_id: 0, $searchScore: 0.9},
                                {_id: 1, $searchScore: 0.8},
                            ],
                            NumberLong(0),
                            coll.getFullName(),
                            1,
                        ),
                    },
                ],
            });
        }

        // Declared bounds: $extensionLimit reports {effect: "limit", value: 50} via its
        // LogicalAggStage get_docs_needed_bounds callback. Bounds become (50, 50); with
        // oversubscriptionFactor=1, batchSize = 50
        {
            const expectedBatchSize = 50;
            primeMongot(expectedBatchSize);
            assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$search: searchQuery}, {$extensionLimit: 50}],
                    cursor: {},
                }),
            );
            mongotMock.assertEmpty();
        }

        // Default (unknown) bounds: $testFoo does not override getDocsNeededBounds, so the callback
        // returns an empty BSONObj which causes the host to treat $testFoo as a stage with unknown
        // bounds. Reverse walk: $limit(5) -> (5, 5); $testFoo resets both bounds to Unknown.
        // batchSize falls back to kDefaultMongotBatchSize.
        {
            primeMongot(kDefaultMongotBatchSize);
            assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$search: searchQuery}, {$testFoo: {}}, {$limit: 5}],
                    cursor: {},
                }),
            );
            mongotMock.assertEmpty();
        }

        // Reverse walk: $limit(50) -> (50, 50); $extensionLimit applies min(25, 50) = 25 to both
        // bounds; $skip(10) adds 10 to each -> (35, 35). batchSize = 35.
        {
            const expectedBatchSize = 35;
            primeMongot(expectedBatchSize);
            assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$search: searchQuery}, {$skip: 10}, {$extensionLimit: 25}, {$limit: 50}],
                    cursor: {},
                }),
            );
            mongotMock.assertEmpty();
        }
    },
    ["standalone"],
);
