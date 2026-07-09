/**
 * Tests that $search (which desugars into $_internalSearchIdLookup) correctly handles documents
 * when chunk migrations occur during query execution. Documents that are migrated mid-query should
 * not be returned from both the source and destination shards (no duplicates), and documents
 * should not be lost.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {ShardingTestWithMongotMock} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const kFailpointName = "hangBeforeResultsInInternalSearchIdLookup";

/**
 * Validates that the results contain all expected IDs with no duplicates.
 */
function validateResults(resultIds, expectedIds) {
    // Verify no duplicates.
    const uniqueIds = [...new Set(resultIds)];
    assert.eq(
        resultIds.length,
        uniqueIds.length,
        `Found duplicate documents in results. IDs: ${tojson(resultIds)}`,
    );

    // Verify all expected documents are returned.
    assert.eq(
        resultIds.length,
        expectedIds.length,
        `Expected ${expectedIds.length} results but got ${resultIds.length}. ` +
            `Missing: ${tojson(expectedIds.filter((id) => !resultIds.includes(id)))}`,
    );
    for (const expectedId of expectedIds) {
        assert(
            resultIds.includes(expectedId),
            `Results should include document with _id=${expectedId}`,
        );
    }
}

/**
 * Sets up fresh mongot mock responses for a single $search execution: each shard's mongot returns
 * every target _id, and mongos gets a planShardedSearch response. mongot responses are consumed per
 * query, so this must be called before every query run.
 */
function setupMongotResponses(ctx) {
    for (const shardConn of [ctx.shard0Conn, ctx.shard1Conn]) {
        const history = [
            {
                expectedCommand: ctx.expectedCommand,
                response: mongotMultiCursorResponseForBatch(
                    ctx.searchBatch,
                    NumberLong(0),
                    [{val: 1}],
                    NumberLong(0),
                    ctx.collNS,
                    1 /* ok */,
                ),
            },
        ];
        const mongot = ctx.stWithMock.getMockConnectedToHost(shardConn);
        mongot.setMockResponses(history, ctx.cursorId, ctx.cursorId + 1000);
        ctx.cursorId++;
    }

    mockPlanShardedSearchResponse(
        ctx.collName,
        ctx.mongotQuery,
        ctx.dbName,
        undefined /* sortSpec */,
        ctx.stWithMock,
    );
}

describe("$search $_internalSearchIdLookup with chunk migrations", function () {
    const dbName = "test";
    const collName = "internal_search_id_lookup_chunk_migration";

    before(function () {
        this.stWithMock = new ShardingTestWithMongotMock({
            name: "id_lookup_chunk_migration",
            shards: {
                rs0: {nodes: 1},
                rs1: {nodes: 1},
            },
            mongos: 1,
        });
        this.stWithMock.start();
        this.st = this.stWithMock.st;

        this.dbName = dbName;
        this.collName = collName;
        this.testDB = this.st.s.getDB(dbName);
        this.testColl = this.testDB.getCollection(collName);
        this.collNS = this.testColl.getFullName();

        assert.commandWorked(
            this.testDB.adminCommand({enableSharding: dbName, primaryShard: this.st.shard0.name}),
        );

        // Insert target documents - 50 on shard0 (shardKey 0-49), 50 on shard1 (shardKey 200-249).
        this.expectedIds = [];
        const docs = [];
        for (let i = 0; i < 50; i++) {
            docs.push({_id: i, shardKey: i, value: `doc${i}`});
            this.expectedIds.push(i);
        }
        for (let i = 200; i < 250; i++) {
            docs.push({_id: i, shardKey: i, value: `doc${i}`});
            this.expectedIds.push(i);
        }
        assert.commandWorked(this.testColl.insertMany(docs));

        // Shard the collection and set up chunk layout.
        // After sharding and splits:
        //   shard0: [minKey, 15)   - target docs 0-14
        //   shard0: [15, 200)      - target docs 15-49 (will be migrated during test)
        //   shard1: [200, maxKey)  - target docs 200-249
        assert.commandWorked(this.testColl.createIndex({shardKey: 1}));
        this.st.shardColl(
            this.testColl,
            {shardKey: 1},
            {shardKey: 200},
            {shardKey: 200},
            dbName,
            true /* waitForDelete */,
        );

        // Split at 15 to prepare for migration during test.
        assert.commandWorked(
            this.testDB.adminCommand({split: this.collNS, middle: {shardKey: 15}}),
        );

        // Stop the balancer to prevent automatic chunk migrations.
        assert.commandWorked(this.testDB.adminCommand({balancerStop: 1}));

        this.shard0Conn = this.st.rs0.getPrimary();
        this.shard1Conn = this.st.rs1.getPrimary();
        this.shard1Coll = this.shard1Conn.getDB(dbName)[collName];

        // A sharded collection shares one UUID across shards.
        this.collUUID = getUUIDFromListCollections(this.shard0Conn.getDB(dbName), collName);

        // Each shard's mongot returns every target _id; $_internalSearchIdLookup looks each up
        // locally and shard filtering keeps only the owned ones, so every target document is
        // returned by exactly one shard. The exact $searchScore values are irrelevant here (results
        // are compared as a set), so any distinct descending scores suffice.
        this.mongotQuery = {};
        this.searchBatch = this.expectedIds.map((id, i) => ({
            _id: id,
            $searchScore: this.expectedIds.length - i,
        }));
        this.expectedCommand = mongotCommandForQuery({
            query: this.mongotQuery,
            collName: collName,
            db: dbName,
            collectionUUID: this.collUUID,
            protocolVersion: NumberInt(1),
        });

        this.pipeline = [{$search: this.mongotQuery}, {$sort: {_id: 1}}];

        // mongot cursor ids used across query runs (responses are consumed per query).
        this.cursorId = 100;
    });

    after(function () {
        this.stWithMock.stop();
    });

    it("returns correct results", function () {
        setupMongotResponses(this);
        const results = this.testColl.aggregate(this.pipeline).toArray();
        const resultIds = results.map((doc) => doc._id);
        validateResults(resultIds, this.expectedIds);
    });

    it("returns correct results when chunk migrates during query", function () {
        // Prime mongot for the query the parallel shell is about to run.
        setupMongotResponses(this);

        // Set up failpoints on both shards to pause the query before idLookup returns.
        const shard0Fp = configureFailPoint(this.shard0Conn, kFailpointName);
        const shard1Fp = configureFailPoint(this.shard1Conn, kFailpointName);

        // Prepare parallel shell to run the query.
        const mongosHost = this.st.s.host;
        const resultCollName = "chunk_migration_test_results";
        const pipelineStr = tojson(this.pipeline);

        const shellCode = `
            const conn = new Mongo("${mongosHost}");
            const coll = conn.getDB("${dbName}").getCollection("${collName}");
            const resultColl = conn.getDB("${dbName}").getCollection("${resultCollName}");
            resultColl.drop();
            const results = coll.aggregate(${pipelineStr}).toArray();
            for (let i = 0; i < results.length; i++) {
                resultColl.insert({_id: i, doc: results[i]});
            }
        `;

        // Start the query in a parallel shell.
        const awaitQueryShell = startParallelShell(shellCode, this.st.s.port);

        // Wait for failpoints to be hit on both shards.
        shard0Fp.wait();
        shard1Fp.wait();

        // While query is paused, migrate chunk [15, 200) from shard0 to shard1.
        // This migrates target documents with shardKey 15-49 (35 documents).
        //   shard0: [minKey, 15)   - target docs 0-14
        //   shard1: [15, 200)      - target docs 15-49 (migrated)
        //   shard1: [200, maxKey)  - target docs 200-249
        assert.commandWorked(
            this.testDB.adminCommand({
                moveChunk: this.collNS,
                find: {shardKey: 20},
                to: this.st.shard1.shardName,
                _waitForDelete: false,
            }),
        );

        // Resume the query.
        shard0Fp.off();
        shard1Fp.off();

        // Wait for query to complete.
        awaitQueryShell();

        // Verify the chunk migration happened: all target docs with shardKey 15-49 should now be
        // on shard1.
        for (let targetDocId = 15; targetDocId < 50; targetDocId++) {
            const docOnShard1 = this.shard1Coll.findOne({_id: targetDocId});
            assert.neq(
                docOnShard1,
                null,
                `Target doc with _id=${targetDocId} should be on shard1 after migration`,
            );
        }

        // Read results from temp collection and validate.
        const resultColl = this.testDB.getCollection(resultCollName);
        const results = resultColl
            .find()
            .sort({_id: 1})
            .toArray()
            .map((r) => r.doc);
        const resultIds = results.map((doc) => doc._id);
        validateResults(resultIds, this.expectedIds);
    });

    it("returns correct results after chunk migration", function () {
        setupMongotResponses(this);
        const results = this.testColl.aggregate(this.pipeline).toArray();
        const resultIds = results.map((doc) => doc._id);
        validateResults(resultIds, this.expectedIds);
    });
});
