/**
 * Verifies the 'clearJoinPlanCache' command drops every entry from the node-global join plan cache,
 * and enforces its restrictions (knob-gated, admin-only, collectionless). Cache contents are always
 * observed through $joinPlanCacheStats. In a sharded cluster the command is broadcast from the
 * router to every shard, while a direct-shard connection clears only that shard.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_replication,
 *   requires_sbe,
 *   requires_sharding,
 * ]
 */

import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

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

// Runs 'pipeline' on 'baseColl' for real, populating the join plan cache. Explain alone never
// populates the cache, so it is only used here as a sanity check that the join is cache-eligible.
function populateCache(baseColl, pipeline) {
    const explain = baseColl.explain().aggregate(pipeline);
    assert(joinOptUsed(explain), "expected join optimization to be used", {explain});
    baseColl.aggregate(pipeline).toArray();
}

describe("clearJoinPlanCache", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({setParameter: joinParams});

        this.testDB = this.conn.getDB(jsTestName());
        this.adminDB = this.conn.getDB("admin");

        const {baseColl, foreignColl, pipeline} = setupJoinFixture(this.testDB);
        this.baseColl = baseColl;
        this.foreignColl = foreignColl;
        this.pipeline = pipeline;

        this.dumpJoinPlanCacheStats = () =>
            this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
        this.clearJoinPlanCache = () => this.adminDB.runCommand({clearJoinPlanCache: 1});
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    // Leave every test case with an empty cache so cases do not depend on each other's residue.
    afterEach(function () {
        assert.commandWorked(this.clearJoinPlanCache());
    });

    it("succeeds on an empty cache", function () {
        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());

        // Clearing again is idempotent.
        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());
    });

    it("clears a populated cache", function () {
        populateCache(this.baseColl, this.pipeline);
        assert.eq(1, this.dumpJoinPlanCacheStats().length);

        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());
    });

    it("clears every entry, not just one", function () {
        // A second, distinct join shape: the same base collection joined to a different foreign
        // collection produces a different cache key.
        const otherForeign = this.testDB.otherForeign;
        assert.commandWorked(
            otherForeign.insertMany([
                {a: 1, d: 1},
                {a: 2, d: 2},
            ]),
        );
        assert.commandWorked(otherForeign.createIndex({a: 1}));
        const otherPipeline = [
            {$match: {a: {$gt: 0}}},
            {$lookup: {from: otherForeign.getName(), localField: "a", foreignField: "a", as: "f"}},
            {$unwind: "$f"},
        ];

        populateCache(this.baseColl, this.pipeline);
        populateCache(this.baseColl, otherPipeline);
        const stats = this.dumpJoinPlanCacheStats();
        assert.eq(2, stats.length, "expected two distinct cache entries", {stats});

        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());

        assert(otherForeign.drop());
    });

    it("leaves the cache usable after clearing", function () {
        populateCache(this.baseColl, this.pipeline);
        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());

        // The memory budget must have been released along with the entries, so the cache can be
        // repopulated as normal.
        populateCache(this.baseColl, this.pipeline);
        assert.eq(1, this.dumpJoinPlanCacheStats().length);
    });

    it("does not disturb the classic plan cache", function () {
        // Populate the classic/SBE plan cache with a plain find, and the join cache with a join. The
        // find needs at least two candidate indexes so that it multi-plans and is cached at all.
        assert.commandWorked(this.baseColl.createIndex({b: 1}));
        this.baseColl.find({a: 1, b: 1}).itcount();
        const classicBefore = this.baseColl.aggregate([{$planCacheStats: {}}]).toArray();
        assert.gt(classicBefore.length, 0, "expected a classic plan cache entry", {classicBefore});
        populateCache(this.baseColl, this.pipeline);

        assert.commandWorked(this.clearJoinPlanCache());
        assert.eq([], this.dumpJoinPlanCacheStats());
        assert.sameMembers(
            classicBefore.map((entry) => entry.planCacheKey),
            this.baseColl
                .aggregate([{$planCacheStats: {}}])
                .toArray()
                .map((entry) => entry.planCacheKey),
            "clearJoinPlanCache must not touch the classic plan cache",
        );

        // And the converse: clearing the collection's plan cache leaves the join cache alone.
        populateCache(this.baseColl, this.pipeline);
        assert.commandWorked(this.testDB.runCommand({planCacheClear: this.baseColl.getName()}));
        assert.eq(1, this.dumpJoinPlanCacheStats().length);
        assert.eq([], this.baseColl.aggregate([{$planCacheStats: {}}]).toArray());

        assert.commandWorked(this.baseColl.dropIndex({b: 1}));
    });

    it("fails when run against a non-admin database", function () {
        assert.commandFailedWithCode(
            this.testDB.runCommand({clearJoinPlanCache: 1}),
            ErrorCodes.Unauthorized,
        );
    });

    it("fails on an unknown field", function () {
        assert.commandFailedWithCode(
            this.adminDB.runCommand({clearJoinPlanCache: 1, bogus: 1}),
            ErrorCodes.IDLUnknownField,
        );
    });

    it("fails when the join plan cache knob is disabled", function () {
        assert.commandWorked(
            this.adminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
        );
        try {
            assert.commandFailedWithCode(
                this.clearJoinPlanCache(),
                ErrorCodes.QueryFeatureNotAllowed,
            );
        } finally {
            assert.commandWorked(
                this.adminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: true}),
            );
        }
    });

    it("fails when the join optimization knob is disabled", function () {
        assert.commandWorked(
            this.adminDB.runCommand({setParameter: 1, internalEnableJoinOptimization: false}),
        );
        try {
            assert.commandFailedWithCode(
                this.clearJoinPlanCache(),
                ErrorCodes.QueryFeatureNotAllowed,
            );
        } finally {
            assert.commandWorked(
                this.adminDB.runCommand({setParameter: 1, internalEnableJoinOptimization: true}),
            );
        }
    });
});

describe("clearJoinPlanCache on a secondary", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: joinParams}});
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();
        this.secondary.setSecondaryOk();

        const {baseColl, pipeline} = setupJoinFixture(this.primary.getDB(jsTestName()));
        this.pipeline = pipeline;
        populateCache(baseColl, pipeline);
        this.rst.awaitReplication();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("clears only the node it is sent to", function () {
        const primaryAdminDB = this.primary.getDB("admin");
        const secondaryAdminDB = this.secondary.getDB("admin");
        const dump = (adminDB) => adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();

        // The cache is not replicated, so populate the secondary's own cache with a local read.
        const secondaryBaseColl = this.secondary.getDB(jsTestName()).base;
        populateCache(secondaryBaseColl, this.pipeline);

        assert.eq(1, dump(primaryAdminDB).length);
        assert.eq(1, dump(secondaryAdminDB).length);

        // The command is allowed on a secondary and affects only that node.
        assert.commandWorked(secondaryAdminDB.runCommand({clearJoinPlanCache: 1}));
        assert.eq([], dump(secondaryAdminDB));
        assert.eq(1, dump(primaryAdminDB).length, "the primary's cache must be untouched");
    });
});

describe("clearJoinPlanCache sharded topology", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: joinParams},
            mongosOptions: {setParameter: joinParams},
            // The router broadcasts the command to every shard, which includes the config server,
            // so the config server needs the knobs enabled as well.
            configOptions: {setParameter: joinParams},
        });

        this.routerAdminDB = this.st.s.getDB("admin");
        this.shards = [
            {name: this.st.shard0.shardName, adminDB: this.st.rs0.getPrimary().getDB("admin")},
            {name: this.st.shard1.shardName, adminDB: this.st.rs1.getPrimary().getDB("admin")},
        ];

        // Give each shard its own database so that both shards run a join locally and populate
        // their own join plan cache.
        this.perShardFixtures = this.shards.map((shard, i) => {
            const dbName = `${jsTestName()}_${i}`;
            assert.commandWorked(
                this.st.s.adminCommand({enableSharding: dbName, primaryShard: shard.name}),
            );
            return {shard, ...setupJoinFixture(this.st.s.getDB(dbName))};
        });

        this.populateAllShards = () => {
            for (const {baseColl, pipeline} of this.perShardFixtures) {
                populateCache(baseColl, pipeline);
            }
        };
        this.dumpViaRouter = () =>
            this.routerAdminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
        this.dumpShard = (shard) => shard.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
    });

    after(function () {
        this.st.stop();
    });

    afterEach(function () {
        assert.commandWorked(this.routerAdminDB.runCommand({clearJoinPlanCache: 1}));
    });

    it("broadcasts to every shard", function () {
        this.populateAllShards();
        const before = this.dumpViaRouter();
        assert.sameMembers(
            this.shards.map((shard) => shard.name),
            [...new Set(before.map((entry) => entry.shard))],
            "expected an entry from each shard before clearing",
        );

        assert.commandWorked(this.routerAdminDB.runCommand({clearJoinPlanCache: 1}));

        assert.eq([], this.dumpViaRouter());
        for (const shard of this.shards) {
            assert.eq([], this.dumpShard(shard), `shard '${shard.name}' cache was not cleared`);
        }
    });

    it("clears only one node when sent to a shard directly", function () {
        this.populateAllShards();
        const [firstShard, secondShard] = this.shards;
        assert.eq(1, this.dumpShard(firstShard).length);
        assert.eq(1, this.dumpShard(secondShard).length);

        assert.commandWorked(firstShard.adminDB.runCommand({clearJoinPlanCache: 1}));

        assert.eq([], this.dumpShard(firstShard));
        assert.eq(1, this.dumpShard(secondShard).length, "the other shard must be untouched");

        // The router's union reflects the surviving shard only.
        assert.eq(
            [secondShard.name],
            this.dumpViaRouter().map((entry) => entry.shard),
        );
    });

    it("fails when run against a non-admin database on the router", function () {
        assert.commandFailedWithCode(
            this.st.s.getDB(jsTestName()).runCommand({clearJoinPlanCache: 1}),
            ErrorCodes.Unauthorized,
        );
    });

    it("fails when the knob is disabled on the router, leaving shard caches intact", function () {
        this.populateAllShards();
        assert.commandWorked(
            this.routerAdminDB.runCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
        );
        try {
            assert.commandFailedWithCode(
                this.routerAdminDB.runCommand({clearJoinPlanCache: 1}),
                ErrorCodes.QueryFeatureNotAllowed,
            );
            // The command must have been rejected before reaching any shard.
            for (const shard of this.shards) {
                assert.eq(1, this.dumpShard(shard).length, {shard: shard.name});
            }
        } finally {
            assert.commandWorked(
                this.routerAdminDB.runCommand({
                    setParameter: 1,
                    internalEnableJoinPlanCache: true,
                }),
            );
        }
    });

    it("is not permitted inside a transaction", function () {
        const session = this.st.s.startSession();
        try {
            session.startTransaction();
            assert.commandFailed(session.getDatabase("admin").runCommand({clearJoinPlanCache: 1}));
            session.abortTransaction();
        } finally {
            session.endSession();
        }
    });
});
