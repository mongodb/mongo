/**
 * Tests that the $_internalSearchIdLookup stage has shard filtering applied to it so that
 * orphaned documents are not returned.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Verifies that orphaned documents exist on their respective shards when queried directly,
 * but are filtered out when querying via mongos.
 */
function verifyOrphansFilteredViaMongos({coll, directShard0Coll, directShard1Coll, shard0OrphanIds, shard1OrphanIds}) {
    const directCountOnShard0 = directShard0Coll.find({_id: {$in: shard0OrphanIds}}).itcount();
    assert.eq(
        directCountOnShard0,
        shard0OrphanIds.length,
        "Orphaned documents should exist on shard0 when queried directly",
    );

    const directCountOnShard1 = directShard1Coll.find({_id: {$in: shard1OrphanIds}}).itcount();
    assert.eq(
        directCountOnShard1,
        shard1OrphanIds.length,
        "Orphaned documents should exist on shard1 when queried directly",
    );

    const allOrphanIds = shard0OrphanIds.concat(shard1OrphanIds);
    const mongosCount = coll.find({_id: {$in: allOrphanIds}}).itcount();
    assert.eq(mongosCount, 0, "Orphaned documents should be filtered out when querying via mongos");
}

describe("$_internalSearchIdLookup shard filtering", function () {
    const dbName = "test";
    const collName = "internal_search_id_lookup_shard_filter";

    before(function () {
        // This test deliberately creates orphans to test shard filtering.
        TestData.skipCheckOrphans = true;

        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
        });

        const mongos = this.st.s;
        this.testDB = mongos.getDB(dbName);
        this.testColl = this.testDB.getCollection(collName);

        assert.commandWorked(this.testDB.adminCommand({enableSharding: dbName, primaryShard: this.st.shard0.name}));

        // Documents that end up on shard0.
        for (const id of [1, 2, 3, 4, 5, 6, 7, 8]) {
            this.testColl.insert({_id: id, shardKey: 0});
        }
        // Documents that end up on shard1.
        for (const id of [11, 12, 13, 14, 15.5, 16, 16.5, 17.5]) {
            this.testColl.insert({_id: id, shardKey: 100});
        }

        // Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to
        // shard1.
        assert.commandWorked(this.testColl.createIndex({shardKey: 1}));

        // 'waitForDelete' is set to 'true' so that range deletion completes before we insert our
        // orphan.
        this.st.shardColl(
            this.testColl,
            {shardKey: 1},
            {shardKey: 10},
            {shardKey: 10 + 1},
            dbName,
            true /* waitForDelete */,
        );

        this.shard0Conn = this.st.rs0.getPrimary();
        this.shard1Conn = this.st.rs1.getPrimary();

        // Store direct shard connections for verification.
        this.shard0Coll = this.shard0Conn.getDB(dbName)[collName];
        this.shard1Coll = this.shard1Conn.getDB(dbName)[collName];

        // Store orphan IDs for verification.
        this.shard0OrphanIds = [15, 18];
        this.shard1OrphanIds = [17, 19];

        // Insert orphan documents into shard 0 which are not owned by that shard.
        for (const id of this.shard0OrphanIds) {
            this.shard0Coll.insert({_id: id, shardKey: 100});
        }

        // Insert orphan documents into shard 1 which are not owned by that shard.
        for (const id of this.shard1OrphanIds) {
            this.shard1Coll.insert({_id: id, shardKey: 0});
        }

        // Verify that the orphaned documents exist on their respective shards, but get filtered out
        // when querying 'testColl'.
        verifyOrphansFilteredViaMongos({
            coll: this.testColl,
            directShard0Coll: this.shard0Coll,
            directShard1Coll: this.shard1Coll,
            shard0OrphanIds: this.shard0OrphanIds,
            shard1OrphanIds: this.shard1OrphanIds,
        });
        assert.eq(this.testColl.find().itcount(), 16);

        this.collName = collName;
        this.dbName = dbName;
    });

    after(function () {
        this.st.stop();
    });

    it("filters out orphan documents", function () {
        // $_internalSearchIdLookup looks up documents by _id locally on each shard. We can't use
        // $match to target orphans directly because $match routes based on shardKey, missing
        // orphans entirely (e.g., orphan _id:15 with shardKey:100 lives on shard0, but $match
        // routes to shard1). Instead, we insert "lookup source" documents on each shard with a
        // 'lookupId' field, then use $project to rename it to '_id' before
        // $_internalSearchIdLookup.
        //
        // Lookup source documents: each entry is {shardKey, lookupId}.
        // lookupIds 15, 17, 18, 19 reference orphan documents.
        const shard0LookupSources = [
            {shardKey: 0, lookupId: 2},
            {shardKey: 0, lookupId: 15}, // orphan
            {shardKey: 1, lookupId: 3},
            {shardKey: 2, lookupId: 18}, // orphan
            {shardKey: 3, lookupId: 4},
            {shardKey: 4, lookupId: 5},
            {shardKey: 5, lookupId: 6},
            {shardKey: 6, lookupId: 7},
            {shardKey: 7, lookupId: 8},
        ];
        const shard1LookupSources = [
            {shardKey: 100, lookupId: 11},
            {shardKey: 101, lookupId: 17}, // orphan
            {shardKey: 102, lookupId: 12},
            {shardKey: 103, lookupId: 19}, // orphan
            {shardKey: 104, lookupId: 13},
            {shardKey: 105, lookupId: 14},
            {shardKey: 106, lookupId: 15.5},
            {shardKey: 107, lookupId: 16},
            {shardKey: 108, lookupId: 16.5},
            {shardKey: 109, lookupId: 17.5},
        ];
        const lookupSources = [...shard0LookupSources, ...shard1LookupSources];
        const baseId = 1000;
        for (let i = 0; i < lookupSources.length; i++) {
            const {shardKey, lookupId} = lookupSources[i];
            this.testColl.insert({_id: baseId + i, shardKey, lookupId});
        }

        const pipeline = [
            {$match: {_id: {$gte: 1000, $lte: 1018}}},
            {$project: {_id: "$lookupId"}},
            {$_internalSearchIdLookup: {}},
            {$sort: {_id: 1}},
        ];

        // Expected docs - orphans (_id: 15, 17, 18, 19) should NOT appear.
        const expectedDocs = [
            {_id: 2, shardKey: 0},
            {_id: 3, shardKey: 0},
            {_id: 4, shardKey: 0},
            {_id: 5, shardKey: 0},
            {_id: 6, shardKey: 0},
            {_id: 7, shardKey: 0},
            {_id: 8, shardKey: 0},
            {_id: 11, shardKey: 100},
            {_id: 12, shardKey: 100},
            {_id: 13, shardKey: 100},
            {_id: 14, shardKey: 100},
            {_id: 15.5, shardKey: 100},
            {_id: 16, shardKey: 100},
            {_id: 16.5, shardKey: 100},
            {_id: 17.5, shardKey: 100},
        ];

        const result = this.testColl.aggregate(pipeline).toArray();
        assert.eq(result, expectedDocs, "Orphan documents should be filtered out by $_internalSearchIdLookup");
    });

    it("filters orphans across getMore batches", function () {
        // Same setup as above: insert lookup source documents that reference both legitimate
        // documents and orphans. This test uses a small batch size to ensure orphan filtering
        // works correctly when results span multiple getMore calls.
        //
        // Lookup source documents: each entry is {shardKey, lookupId}.
        // lookupIds 15, 17, 18, 19 reference orphan documents.
        const shard0LookupSources = [
            {shardKey: 0, lookupId: 2},
            {shardKey: 0, lookupId: 15}, // orphan
            {shardKey: 1, lookupId: 3},
            {shardKey: 2, lookupId: 18}, // orphan
            {shardKey: 3, lookupId: 4},
            {shardKey: 4, lookupId: 5},
            {shardKey: 5, lookupId: 6},
            {shardKey: 6, lookupId: 7},
            {shardKey: 7, lookupId: 8},
        ];
        const shard1LookupSources = [
            {shardKey: 100, lookupId: 11},
            {shardKey: 101, lookupId: 17}, // orphan
            {shardKey: 102, lookupId: 12},
            {shardKey: 103, lookupId: 19}, // orphan
            {shardKey: 104, lookupId: 13},
            {shardKey: 105, lookupId: 14},
            {shardKey: 106, lookupId: 15.5},
            {shardKey: 107, lookupId: 16},
            {shardKey: 108, lookupId: 16.5},
            {shardKey: 109, lookupId: 17.5},
        ];
        const lookupSources = [...shard0LookupSources, ...shard1LookupSources];
        const baseId = 4000;
        for (let i = 0; i < lookupSources.length; i++) {
            const {shardKey, lookupId} = lookupSources[i];
            this.testColl.insert({_id: baseId + i, shardKey, lookupId});
        }

        const pipeline = [
            {$match: {_id: {$gte: 4000, $lte: 4018}}},
            {$project: {_id: "$lookupId"}},
            {$_internalSearchIdLookup: {}},
            {$sort: {_id: 1}},
        ];

        // Expected docs - orphans (_id: 15, 17, 18, 19) should NOT appear.
        const expectedDocs = [
            {_id: 2, shardKey: 0},
            {_id: 3, shardKey: 0},
            {_id: 4, shardKey: 0},
            {_id: 5, shardKey: 0},
            {_id: 6, shardKey: 0},
            {_id: 7, shardKey: 0},
            {_id: 8, shardKey: 0},
            {_id: 11, shardKey: 100},
            {_id: 12, shardKey: 100},
            {_id: 13, shardKey: 100},
            {_id: 14, shardKey: 100},
            {_id: 15.5, shardKey: 100},
            {_id: 16, shardKey: 100},
            {_id: 16.5, shardKey: 100},
            {_id: 17.5, shardKey: 100},
        ];

        const resultWithSmallBatch = this.testColl.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
        assert.eq(resultWithSmallBatch, expectedDocs, "Orphan filtering should work across getMore batches");
    });

    it("filters orphans in $lookup subpipeline", function () {
        // Test that orphan filtering works when $_internalSearchIdLookup is used inside a $lookup
        // subpipeline. This simulates running $search inside a $lookup.
        const baseCollName = jsTestName() + "_lookup_base";
        const baseColl = this.testDB.getCollection(baseCollName);
        baseColl.drop();
        assert.commandWorked(baseColl.insert({_id: 100}));
        assert.commandWorked(baseColl.insert({_id: 200}));
        this.st.shardColl(baseColl, {_id: 1}, {_id: 150}, {_id: 150});

        // Insert lookup source documents. lookupId: 3 and 11 reference legitimate documents,
        // while lookupId: 15 and 17 reference orphaned documents (created in before()).
        const lookupSources = [
            {_id: 2000, shardKey: 4, lookupId: 15}, // orphan
            {_id: 2001, shardKey: 5, lookupId: 3},
            {_id: 2002, shardKey: 103, lookupId: 17}, // orphan
            {_id: 2003, shardKey: 104, lookupId: 11},
        ];
        this.testColl.insertMany(lookupSources);

        const lookupPipeline = [
            {
                $lookup: {
                    from: this.collName,
                    pipeline: [
                        {$match: {_id: {$gte: 2000, $lte: 2003}}},
                        {$project: {_id: "$lookupId"}},
                        {$_internalSearchIdLookup: {}},
                        {$sort: {_id: 1}},
                    ],
                    as: "lookedUpDocs",
                },
            },
            {$sort: {_id: 1}},
        ];

        // Orphans (_id: 15, 17) should not appear.
        const expectedLookupResults = [
            {
                _id: 100,
                lookedUpDocs: [
                    {_id: 3, shardKey: 0},
                    {_id: 11, shardKey: 100},
                ],
            },
            {
                _id: 200,
                lookedUpDocs: [
                    {_id: 3, shardKey: 0},
                    {_id: 11, shardKey: 100},
                ],
            },
        ];

        const lookupResult = baseColl.aggregate(lookupPipeline).toArray();
        assert.eq(lookupResult, expectedLookupResults, "$lookup should filter orphans");
    });

    it("filters orphans in $unionWith subpipeline", function () {
        // Test that orphan filtering works when $_internalSearchIdLookup is used inside a
        // $unionWith subpipeline. This simulates running $search inside a $unionWith.
        const baseCollName = jsTestName() + "_union_base";
        const baseColl = this.testDB.getCollection(baseCollName);
        baseColl.drop();
        assert.commandWorked(baseColl.insert({_id: 100}));
        assert.commandWorked(baseColl.insert({_id: 200}));
        this.st.shardColl(baseColl, {_id: 1}, {_id: 150}, {_id: 150});

        // Insert lookup source documents. lookupId: 2 and 12 reference legitimate documents,
        // while lookupId: 15 and 17 reference orphaned documents (created in before()).
        const lookupSources = [
            {_id: 3000, shardKey: 6, lookupId: 15}, // orphan
            {_id: 3001, shardKey: 7, lookupId: 2},
            {_id: 3002, shardKey: 105, lookupId: 17}, // orphan
            {_id: 3003, shardKey: 106, lookupId: 12},
        ];
        this.testColl.insertMany(lookupSources);

        const unionPipeline = [
            {
                $unionWith: {
                    coll: this.collName,
                    pipeline: [
                        {$match: {_id: {$gte: 3000, $lte: 3003}}},
                        {$project: {_id: "$lookupId"}},
                        {$_internalSearchIdLookup: {}},
                    ],
                },
            },
            {$sort: {_id: 1}},
        ];

        // Orphans (_id: 15, 17) should not appear.
        const expectedUnionDocs = [{_id: 2, shardKey: 0}, {_id: 12, shardKey: 100}, {_id: 100}, {_id: 200}];

        const unionResult = baseColl.aggregate(unionPipeline).toArray();
        assert.eq(unionResult, expectedUnionDocs, "$unionWith should filter orphans");
    });

    it("orphans still exist on shards but are filtered via mongos", function () {
        verifyOrphansFilteredViaMongos({
            coll: this.testColl,
            directShard0Coll: this.shard0Coll,
            directShard1Coll: this.shard1Coll,
            shard0OrphanIds: this.shard0OrphanIds,
            shard1OrphanIds: this.shard1OrphanIds,
        });
    });
});
