/**
 * Verifies the $joinPlanCacheStats aggregation stage dumps the contents of the node-global join
 * plan cache, and enforces its restrictions (knob-gated, collectionless against 'admin'). In a
 * sharded cluster the stage is forwarded to every shard and the per-shard results are unioned, with
 * each entry tagged with the shard it came from.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 *   requires_sharding,
 * ]
 */

import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {tojsonMultiLineSortKeys} from "jstests/libs/query_optimization/golden_test.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const joinParams = {
    internalEnableJoinOptimization: true,
    internalEnableJoinPlanCache: true,
};

// Populates 'db' with a 'base' and a 'foreign' collection, both indexed on 'a', and returns them
// along with a cache-eligible $lookup/$unwind join pipeline over them.
function setupJoinFixture(db) {
    const baseColl = db.base;
    const foreignColl = db.foreign;

    assert.commandWorked(
        baseColl.insertMany([
            {a: 1, b: 1},
            {a: 2, b: 2},
            {a: 3, b: 1},
        ]),
    );
    assert.commandWorked(
        foreignColl.insertMany([
            {a: 1, c: "foo"},
            {a: 2, c: "bar"},
            {a: 3, c: "baz"},
        ]),
    );
    assert.commandWorked(baseColl.createIndex({a: 1}));
    assert.commandWorked(foreignColl.createIndex({a: 1}));

    // Flush pending writes so that the collection size estimates the join optimizer costs plans
    // with reflect the on-disk state.
    assert.commandWorked(db.adminCommand({fsync: 1}));

    // A cache-eligible $lookup/$unwind join with an equality join on field 'a'.
    const pipeline = [
        {$match: {a: {$gt: 0}}},
        {
            $lookup: {
                from: foreignColl.getName(),
                localField: "a",
                foreignField: "a",
                as: "f",
            },
        },
        {$unwind: "$f"},
    ];

    return {baseColl, foreignColl, pipeline};
}

// Replaces the non-deterministic fields of a join plan cache entry (a collection's random UUID, the
// allocator-dependent size estimate, and the plan cache key hash) with stable placeholders, so that
// the entry can be compared against the expected output spelled out in "reports the cached join
// plan" below.
function redactEntry(entry) {
    return Object.assign({}, entry, {
        planCacheKey: "<planCacheKey>",
        estimatedSizeBytes: "<int>",
        collections: entry.collections.map((coll) => Object.assign({}, coll, {uuid: "<uuid>"})),
    });
}

// Asserts that 'entry' has the expected shape of a join plan cache entry.
function assertIsJoinPlanCacheEntry(entry) {
    for (const field of ["planCacheKey", "estimatedSizeBytes", "collections", "plan"]) {
        assert(entry.hasOwnProperty(field), `entry missing '${field}'`, {entry});
    }
    assert.eq(typeof entry.planCacheKey, "string", {entry});
    assert(Array.isArray(entry.collections), {entry});
}

describe("$joinPlanCacheStats", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: joinParams,
        });

        this.testDB = this.conn.getDB(jsTestName());
        this.adminDB = this.conn.getDB("admin");

        const {baseColl, foreignColl, pipeline} = setupJoinFixture(this.testDB);
        this.baseColl = baseColl;
        this.foreignColl = foreignColl;
        this.pipeline = pipeline;

        this.dumpJoinPlanCacheStats = () =>
            this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("populates and dumps join plan cache entries after a join query runs", function () {
        // Sanity check that join optimization actually runs for this pipeline.
        const explain = this.baseColl.explain().aggregate(this.pipeline);
        assert(joinOptUsed(explain), "expected join optimization to be used", {explain});

        // Execute the pipeline for real to populate the join plan cache.
        this.baseColl.aggregate(this.pipeline).toArray();

        const stats = this.dumpJoinPlanCacheStats();
        assert.gte(stats.length, 1, "expected at least one join plan cache entry", {stats});

        assertIsJoinPlanCacheEntry(stats[0]);

        // Outside of a sharded cluster there is no shard to attribute entries to.
        assert(!stats[0].hasOwnProperty("shard"), "unexpected 'shard' field", {stats});
    });

    it("fails when run against a collection", function () {
        assert.commandFailedWithCode(
            this.testDB.runCommand({
                aggregate: this.baseColl.getName(),
                pipeline: [{$joinPlanCacheStats: {}}],
                cursor: {},
            }),
            ErrorCodes.InvalidNamespace,
        );
    });

    it("fails when run against a non-admin database with {aggregate: 1}", function () {
        assert.commandFailedWithCode(
            this.testDB.runCommand({
                aggregate: 1,
                pipeline: [{$joinPlanCacheStats: {}}],
                cursor: {},
            }),
            ErrorCodes.InvalidNamespace,
        );
    });

    it("fails when the join plan cache knob is disabled", function () {
        assert.commandWorked(
            this.adminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
        );
        try {
            assert.commandFailedWithCode(
                this.adminDB.runCommand({
                    aggregate: 1,
                    pipeline: [{$joinPlanCacheStats: {}}],
                    cursor: {},
                }),
                ErrorCodes.QueryFeatureNotAllowed,
            );
        } finally {
            assert.commandWorked(
                this.adminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: true}),
            );
        }
    });

    it("supports other stages in the pipeline", function () {
        this.baseColl.aggregate(this.pipeline).toArray();
        const result = this.adminDB
            .aggregate([{$joinPlanCacheStats: {}}, {$match: {collections: {$size: 2}}}])
            .toArray();
        assert.eq(1, result.length);
    });
});

describe("$joinPlanCacheStats output shape", function () {
    // Runs on its own mongod so that the cache contains exactly the one entry this test creates,
    // independent of the order the other tests in this file run in.
    before(function () {
        this.conn = MongoRunner.runMongod({setParameter: joinParams});
        this.adminDB = this.conn.getDB("admin");

        const {baseColl, pipeline} = setupJoinFixture(this.conn.getDB(jsTestName()));
        this.baseColl = baseColl;
        this.pipeline = pipeline;

        this.dumpJoinPlanCacheStats = () =>
            this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("reports an empty cache until a join query runs", function () {
        assert.eq([], this.dumpJoinPlanCacheStats());

        // Explain alone never populates the cache.
        const explain = this.baseColl.explain().aggregate(this.pipeline);
        assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
        assert.eq([], this.dumpJoinPlanCacheStats());
    });

    it("reports the cached join plan", function () {
        this.baseColl.aggregate(this.pipeline).toArray();

        const stats = this.dumpJoinPlanCacheStats();
        assert.eq(1, stats.length, "expected exactly one join plan cache entry", {stats});

        // The full expected output for the fixture pipeline, documenting the stage's output shape
        // in-line. On a mismatch the assertion prints a diff against the actual output.
        assert.eq(
            `[
	{
		"baseNode" : 0,
		"collections" : [
			{
				"collectionVersion" : NumberLong(2),
				"sampleVersion" : NumberLong(0),
				"uuid" : "<uuid>"
			},
			{
				"collectionVersion" : NumberLong(2),
				"sampleVersion" : NumberLong(0),
				"uuid" : "<uuid>"
			}
		],
		"estimatedSizeBytes" : "<int>",
		"plan" : {
			"joinMethod" : "HJ",
			"joinPredicates" : [
				"a = a"
			],
			"left" : {
				"accessPath" : "(collection scan)",
				"nodeId" : 0
			},
			"right" : {
				"accessPath" : "(collection scan)",
				"nodeId" : 1
			},
			"rightEmbeddingField" : "f"
		},
		"planCacheKey" : "<planCacheKey>"
	}
]`,
            tojsonMultiLineSortKeys(stats.map(redactEntry)),
            "unexpected $joinPlanCacheStats output",
        );
    });
});

describe("$joinPlanCacheStats sharded topology", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {
                nodes: 1,
                setParameter: joinParams,
            },
            mongosOptions: {
                setParameter: joinParams,
            },
        });

        this.routerAdminDB = this.st.s.getDB("admin");
        this.shards = [
            {name: this.st.shard0.shardName, adminDB: this.st.rs0.getPrimary().getDB("admin")},
            {name: this.st.shard1.shardName, adminDB: this.st.rs1.getPrimary().getDB("admin")},
        ];

        // Give each shard its own database so that both shards run a join locally and populate
        // their own join plan cache. Join optimization requires the join to execute on a shard, so
        // this is more deterministic than relying on $lookup pushdown for a sharded collection.
        this.perShardFixtures = this.shards.map((shard, i) => {
            const dbName = `${jsTestName()}_${i}`;
            assert.commandWorked(
                this.st.s.adminCommand({enableSharding: dbName, primaryShard: shard.name}),
            );
            const fixture = setupJoinFixture(this.st.s.getDB(dbName));
            return {shard, ...fixture};
        });

        // Run the join on each shard's database so every shard caches a plan.
        for (const {baseColl, pipeline} of this.perShardFixtures) {
            const explain = baseColl.explain().aggregate(pipeline);
            assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
            baseColl.aggregate(pipeline).toArray();
        }

        this.dumpJoinPlanCacheStats = () =>
            this.routerAdminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
    });

    after(function () {
        this.st.stop();
    });

    it("forwards to all shards and unions the results", function () {
        const stats = this.dumpJoinPlanCacheStats();
        assert.gte(stats.length, this.shards.length, "expected an entry from each shard", {stats});

        for (const entry of stats) {
            assertIsJoinPlanCacheEntry(entry);
            assert.eq(typeof entry.shard, "string", "entry missing 'shard'", {entry});
        }

        const seenShards = new Set(stats.map((entry) => entry.shard));
        assert.sameMembers(
            this.shards.map((shard) => shard.name),
            [...seenShards],
            `expected results from every shard, got ${tojson(stats)}`,
        );
    });

    it("matches what each shard reports directly", function () {
        const stats = this.dumpJoinPlanCacheStats();

        for (const {name, adminDB} of this.shards) {
            const direct = adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();

            // Run directly against a shard, entries are not attributed to any shard.
            for (const entry of direct) {
                assert(!entry.hasOwnProperty("shard"), "unexpected 'shard' field", {entry});
            }

            assert.sameMembers(
                direct.map((entry) => entry.planCacheKey),
                stats.filter((entry) => entry.shard === name).map((entry) => entry.planCacheKey),
                `router results for shard '${name}' disagree with the shard itself: ` +
                    `${tojson({direct, stats})}`,
            );
        }
    });

    it("supports other stages after it on the router", function () {
        const matched = this.routerAdminDB
            .aggregate([{$joinPlanCacheStats: {}}, {$match: {collections: {$size: 2}}}])
            .toArray();
        assert.eq(this.shards.length, matched.length, {matched});

        // Grouping happens in the merging pipeline on the router, over all the shards' cursors.
        const grouped = this.routerAdminDB
            .aggregate([{$joinPlanCacheStats: {}}, {$group: {_id: "$shard", n: {$sum: 1}}}])
            .toArray();
        assert.sameMembers(
            this.shards.map((shard) => shard.name),
            grouped.map((doc) => doc._id),
            {grouped},
        );
    });

    it("fails when run against a collection", function () {
        const {baseColl} = this.perShardFixtures[0];
        assert.commandFailedWithCode(
            baseColl.getDB().runCommand({
                aggregate: baseColl.getName(),
                pipeline: [{$joinPlanCacheStats: {}}],
                cursor: {},
            }),
            ErrorCodes.InvalidNamespace,
        );
    });

    it("fails when the join plan cache knob is disabled on the router", function () {
        assert.commandWorked(
            this.routerAdminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
        );
        try {
            assert.commandFailedWithCode(
                this.routerAdminDB.runCommand({
                    aggregate: 1,
                    pipeline: [{$joinPlanCacheStats: {}}],
                    cursor: {},
                }),
                ErrorCodes.QueryFeatureNotAllowed,
            );
        } finally {
            assert.commandWorked(
                this.routerAdminDB.runCommand({
                    setParameter: 1,
                    internalEnableJoinPlanCache: true,
                }),
            );
        }
    });
});
