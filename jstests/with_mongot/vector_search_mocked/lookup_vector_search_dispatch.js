/**
 * Tests that $vectorSearch inside a $lookup subpipeline dispatches to mongot the correct number of
 * times: once per input document when the subpipeline cache is disabled, and exactly once when
 * pipeline optimization is on (the $lookup subpipeline cache recognizes the uncorrelated
 * $vectorSearch prefix and serves the cached results for subsequent input documents, just like
 * $search).
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const queryVector = [1.0, 2.0, 3.0];
const path = "plot_embedding";
const numCandidates = 10;
const limit = 3;
const index = "default";
const responseOk = 1;

describe("$vectorSearch dispatch counts inside $lookup", function () {
    let mongotmock;
    let conn;
    let db;
    let foreignColl;
    let localColl;
    let collectionUUID;
    let dbName;
    let collNS;

    // Cursor IDs must be unique across all setMockResponses calls in a single test run.
    let cursorCounter = 100;
    function nextCursorId() {
        return NumberLong(cursorCounter++);
    }

    // The batch that mongot "returns" for each vectorSearch invocation.  The _ids must exist in
    // foreignColl so that $_internalSearchIdLookup can resolve them.
    const mongotBatch = [
        {_id: 1, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.55},
    ];

    before(function () {
        mongotmock = new MongotMock();
        mongotmock.start();
        const mongotConn = mongotmock.getConnection();

        // Enable featureFlagExtensionsInsideHybridSearch at startup so that $vectorSearch is
        // allowed inside $lookup subpipelines.
        // TODO SERVER-121094 Remove when the feature flag is removed.
        conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotConn.host,
                featureFlagExtensionsInsideHybridSearch: true,
            },
        });

        dbName = jsTestName();
        db = conn.getDB(dbName);
        collNS = dbName + ".movies";

        // "foreign" collection that $vectorSearch runs against; also what
        // $_internalSearchIdLookup resolves _ids into.
        foreignColl = db["movies"];
        foreignColl.drop();
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 1, title: "Tarzan the Ape Man"},
                {_id: 2, title: "King Kong"},
                {_id: 3, title: "Mighty Joe Young"},
            ]),
        );

        // "local" collection that drives the outer $lookup – 2 documents so that the per-doc
        // vs. cached cases produce different mongot dispatch counts (when caching works).
        localColl = db["local"];
        localColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 10, tag: "first"},
                {_id: 11, tag: "second"},
            ]),
        );

        collectionUUID = getUUIDFromListCollections(db, foreignColl.getName());
    });

    after(function () {
        MongoRunner.stopMongod(conn);
        mongotmock.stop();
    });

    /**
     * Queue 'times' identical vectorSearch mock responses in mongotmock and return the
     * vectorSearch stage spec to use in the pipeline.
     */
    function setupVectorSearchMock(times) {
        const vectorSearchSpec = {queryVector, path, numCandidates, limit, index};

        const expectedCommand = mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            limit,
            index,
            collName: foreignColl.getName(),
            dbName,
            collectionUUID,
        });

        for (let i = 0; i < times; i++) {
            const history = [
                {
                    expectedCommand,
                    response: mongotResponseForBatch(
                        mongotBatch,
                        NumberLong(0),
                        collNS,
                        responseOk,
                    ),
                },
            ];
            mongotmock.setMockResponses(history, nextCursorId());
        }

        return vectorSearchSpec;
    }

    // $lookup pipeline used in both cases.  The subpipeline is uncorrelated ($vectorSearch has
    // no reference to outer variables).
    function makeLookupPipeline(vectorSearchSpec) {
        return [
            {
                $lookup: {
                    from: foreignColl.getName(),
                    pipeline: [{$vectorSearch: vectorSearchSpec}],
                    as: "results",
                },
            },
            {$sort: {_id: 1}},
        ];
    }

    // Both test cases expect the same per-document result content.
    const expectedMovies = [
        {_id: 1, title: "Tarzan the Ape Man"},
        {_id: 2, title: "King Kong"},
    ];

    it("dispatches once per input doc when disablePipelineOptimization is active", function () {
        // 2 local docs → 2 mongot dispatches expected.
        const vectorSearchSpec = setupVectorSearchMock(2 /* times */);

        assert.commandWorked(
            db.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "alwaysOn"}),
        );

        let results;
        try {
            results = localColl.aggregate(makeLookupPipeline(vectorSearchSpec)).toArray();
        } finally {
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "disablePipelineOptimization", mode: "off"}),
            );
        }

        // Both local docs should be present.
        assert.eq(results.length, 2, "expected 2 output docs");

        // Each output doc's 'results' array must contain the two movies whose _ids were returned
        // by mongot (resolved via $_internalSearchIdLookup against foreignColl).
        for (const doc of results) {
            assert.sameMembers(doc.results, expectedMovies);
        }

        // Verify that exactly 2 dispatches happened: mongotmock queue should now be drained.
        mongotmock.assertEmpty();
    });

    it(
        "dispatches exactly once when pipeline optimizations are on (uncorrelated $vectorSearch is " +
            "cached)",
        function () {
            // Like $search, an uncorrelated $vectorSearch subpipeline is exempted from the
            // sequential document cache's NOT_SUPPORTED dependency check in
            // DocumentSourceSequentialDocumentCache::optimizeAt().  The first input document
            // populates the cache (one mongot dispatch) and the second is served from the cache, so
            // mongot is contacted exactly once even though there are 2 input documents.
            const vectorSearchSpec = setupVectorSearchMock(1 /* times – cache serves the rest */);

            const results = localColl.aggregate(makeLookupPipeline(vectorSearchSpec)).toArray();

            assert.eq(results.length, 2, "expected 2 output docs");

            // Both output docs must carry the full joined results, including the one served from the
            // cache for the second input document.
            for (const doc of results) {
                assert.sameMembers(doc.results, expectedMovies);
            }

            // Exactly 1 dispatch: the single queued mock response was consumed.
            mongotmock.assertEmpty();
        },
    );
});
