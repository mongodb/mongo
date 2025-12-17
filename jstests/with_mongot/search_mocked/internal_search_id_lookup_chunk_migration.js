/**
 * Tests that $_internalSearchIdLookup correctly handles documents when chunk migrations
 * occur during query execution. Documents that are migrated mid-query should not be
 * returned from both the source and destination shards (no duplicates), and documents
 * should not be lost.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kFailpointName = "hangBeforeResultsInInternalSearchIdLookup";

/**
 * Creates a document with the given id and shardKey.
 */
function createDocument(id, shardKey) {
    return {_id: id, shardKey: shardKey, value: `doc${id}`};
}

/**
 * Creates a lookup source document that references a target document.
 */
function createLookupSourceDocument(lookupId, shardKey, targetId) {
    return {_id: lookupId, shardKey: shardKey, lookupId: targetId};
}

/**
 * Validates that the results contain all expected IDs with no duplicates.
 */
function validateResults(resultIds, expectedIds) {
    // Verify no duplicates.
    const uniqueIds = [...new Set(resultIds)];
    assert.eq(resultIds.length, uniqueIds.length, `Found duplicate documents in results. IDs: ${tojson(resultIds)}`);

    // Verify all expected documents are returned.
    assert.eq(
        resultIds.length,
        expectedIds.length,
        `Expected ${expectedIds.length} results but got ${resultIds.length}. ` +
            `Missing: ${tojson(expectedIds.filter((id) => !resultIds.includes(id)))}`,
    );
    for (const expectedId of expectedIds) {
        assert(resultIds.includes(expectedId), `Results should include document with _id=${expectedId}`);
    }
}

describe("$_internalSearchIdLookup with chunk migrations", function () {
    const dbName = "test";
    const collName = "internal_search_id_lookup_chunk_migration";

    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
        });

        const mongos = this.st.s;
        this.testDB = mongos.getDB(dbName);
        this.testColl = this.testDB.getCollection(collName);

        assert.commandWorked(this.testDB.adminCommand({enableSharding: dbName, primaryShard: this.st.shard0.name}));

        // Insert documents - 50 on shard0 (shardKey 0-49), 50 on shard1 (shardKey 200-249).
        this.docs = [];
        for (let i = 0; i < 50; i++) {
            this.docs.push(createDocument(i, i));
        }
        for (let i = 200; i < 250; i++) {
            this.docs.push(createDocument(i, i));
        }
        for (const doc of this.docs) {
            assert.commandWorked(this.testColl.insert(doc));
        }

        // Shard the collection and set up chunk layout.
        // After sharding and splits:
        //   shard0: [minKey, 15)   - target docs 0-14, lookup sources shardKey -100
        //   shard0: [15, 200)      - target docs 15-49 (will be migrated during test)
        //   shard1: [200, maxKey)  - target docs 200-249, lookup sources shardKey 500+
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
        assert.commandWorked(this.testDB.adminCommand({split: this.testColl.getFullName(), middle: {shardKey: 15}}));

        // Stop the balancer to prevent automatic chunk migrations.
        assert.commandWorked(this.testDB.adminCommand({balancerStop: 1}));

        this.shard0Conn = this.st.rs0.getPrimary();
        this.shard1Conn = this.st.rs1.getPrimary();
        this.shard0Coll = this.shard0Conn.getDB(dbName)[collName];
        this.shard1Coll = this.shard1Conn.getDB(dbName)[collName];

        // We need to manufacture the _id input to $_internalSearchIdLookup so we insert
        // "lookup source" documents on BOTH shards that reference ALL target documents. This
        // ensures that after a chunk migration, the migrated docs can still be found on either
        // shard.
        this.lookupSourceDocs = [];

        // Lookup sources on shard0 (shardKey -100, stays on shard0 since < 15)
        // Reference ALL target docs: 0-49 and 200-249
        for (let targetDocId = 0; targetDocId < 50; targetDocId++) {
            const lookupId = 1000 + targetDocId;
            const shardKey = -100;
            this.lookupSourceDocs.push(createLookupSourceDocument(lookupId, shardKey, targetDocId));
        }
        for (let targetDocId = 200; targetDocId < 250; targetDocId++) {
            const lookupId = 1000 + targetDocId;
            const shardKey = -100;
            this.lookupSourceDocs.push(createLookupSourceDocument(lookupId, shardKey, targetDocId));
        }

        // Lookup sources on shard1 (shardKey 500/550)
        // Reference ALL target docs: 0-49 and 200-249
        for (let targetDocId = 0; targetDocId < 50; targetDocId++) {
            const lookupId = 2000 + targetDocId;
            const shardKey = 500;
            this.lookupSourceDocs.push(createLookupSourceDocument(lookupId, shardKey, targetDocId));
        }
        for (let targetDocId = 200; targetDocId < 250; targetDocId++) {
            const lookupId = 2000 + targetDocId;
            const shardKey = 550;
            this.lookupSourceDocs.push(createLookupSourceDocument(lookupId, shardKey, targetDocId));
        }

        for (const doc of this.lookupSourceDocs) {
            assert.commandWorked(this.testColl.insert(doc));
        }

        // Pipeline to run $_internalSearchIdLookup on all lookup source documents.
        this.pipeline = [
            {$match: {_id: {$gte: 1000, $lt: 2250}}},
            {$project: {_id: "$lookupId"}},
            {$_internalSearchIdLookup: {}},
            {$sort: {_id: 1}},
        ];

        // Expected IDs - all target documents: 0-49 and 200-249.
        this.expectedIds = this.docs.map((doc) => doc._id);

        this.collName = collName;
        this.dbName = dbName;
    });

    after(function () {
        this.st.stop();
    });

    it("returns correct results", function () {
        const results = this.testColl.aggregate(this.pipeline).toArray();
        const resultIds = results.map((doc) => doc._id);
        validateResults(resultIds, this.expectedIds);
    });

    it("returns correct results when chunk migrates during query", function () {
        // Set up failpoints on both shards to pause the query before idLookup returns.
        const shard0Fp = configureFailPoint(this.shard0Conn, kFailpointName);
        const shard1Fp = configureFailPoint(this.shard1Conn, kFailpointName);

        // Prepare parallel shell to run the query.
        const mongosHost = this.st.s.host;
        const resultCollName = "chunk_migration_test_results";
        const pipelineStr = tojson(this.pipeline);
        const dbName = this.dbName;
        const collName = this.collName;

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
        //   shard0: [minKey, 15)   - target docs 0-14, lookup sources shardKey -100
        //   shard1: [15, 200)      - target docs 15-49 (migrated)
        //   shard1: [200, maxKey)  - target docs 200-249, lookup sources shardKey 500
        assert.commandWorked(
            this.testDB.adminCommand({
                moveChunk: this.testColl.getFullName(),
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
            assert.neq(docOnShard1, null, `Target doc with _id=${targetDocId} should be on shard1 after migration`);
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
        const results = this.testColl.aggregate(this.pipeline).toArray();
        const resultIds = results.map((doc) => doc._id);
        validateResults(resultIds, this.expectedIds);
    });
});
