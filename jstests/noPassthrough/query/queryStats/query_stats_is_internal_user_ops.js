/**
 * Tests that $queryStats marks an operation as isInternal only when it is stamped, and that the
 * stamp follows the operation from mongos to the shards. A client's commands, including the
 * sub-operations the server runs on the client's behalf, are not marked. A client without internal
 * authorization cannot mark itself internal by sending the isInternalOperation generic argument.
 *
 * @tags: [requires_sharding, requires_replication, requires_fcv_91]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";
const foreignCollName = "foreign";
const queryStatsParams = {
    internalQueryStatsSampleRate: 1,
    internalQueryStatsWriteCmdSampleRate: 1,
};

/**
 * Returns every $queryStats entry on 'conn' for the test database. Entries are not filtered on the
 * shell's application name because shard-side entries carry the router's client metadata.
 */
function getEntriesForTestDb(conn) {
    return assert.commandWorked(
        conn.adminCommand({
            aggregate: 1,
            pipeline: [{$queryStats: {}}, {$match: {"key.queryShape.cmdNs.db": dbName}}],
            cursor: {},
        }),
    ).cursor.firstBatch;
}

/**
 * Runs a client's commands, each picked because the server executes part of it on an operation
 * other than the one the client's command arrived on.
 */
function runUserWorkload(conn) {
    const coll = conn.getDB(dbName)[collName];

    // On a sharded cluster, mongos sends these to the shards over internal connections.
    assert.eq(1, coll.find({_id: 1}).itcount());
    assert.commandWorked(coll.updateOne({_id: 1}, {$set: {w: 1}}));
    // The foreign-side reads run as sub-operations of the aggregate.
    assert.eq(
        3,
        coll
            .aggregate([
                {$lookup: {from: foreignCollName, localField: "v", foreignField: "v", as: "f"}},
            ])
            .itcount(),
    );
    // The server writes the results to another collection on the client's behalf.
    coll.aggregate([{$match: {v: {$gte: 0}}}, {$merge: {into: "merged"}}]).itcount();
}

function assertNoInternalEntries(conns) {
    for (const conn of conns) {
        const entries = getEntriesForTestDb(conn);
        assert.gt(entries.length, 0, "Expected query stats entries for the user workload", {
            host: conn.host,
        });
        const internal = entries.filter((e) => e.key.hasOwnProperty("isInternal"));
        assert.eq([], internal, "User operations must NOT be marked isInternal", {host: conn.host});
    }
}

function setUpCollections(conn) {
    const testDB = conn.getDB(dbName);
    assert.commandWorked(
        testDB[collName].insert([
            {_id: 1, v: 1},
            {_id: 2, v: 2},
            {_id: 3, v: 3},
        ]),
    );
    assert.commandWorked(
        testDB[foreignCollName].insert([
            {_id: 1, v: 1},
            {_id: 2, v: 2},
            {_id: 3, v: 3},
        ]),
    );
}

describe("query stats isInternal is absent for user operations", function () {
    it("on a standalone", function () {
        const conn = MongoRunner.runMongod({setParameter: queryStatsParams});
        try {
            setUpCollections(conn);
            resetQueryStatsStore(conn, "1MB");
            runUserWorkload(conn);
            assertNoInternalEntries([conn]);
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    it("on a replica set", function () {
        const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: queryStatsParams}});
        rst.startSet();
        rst.initiate();
        try {
            const primary = rst.getPrimary();
            setUpCollections(primary);
            resetQueryStatsStore(primary, "1MB");
            runUserWorkload(primary);
            assertNoInternalEntries([primary]);
        } finally {
            rst.stopSet();
        }
    });

    it("on a sharded cluster", function () {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });
        try {
            const ns = `${dbName}.${collName}`;
            assert.commandWorked(
                st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
            );
            assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
            assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 2}}));
            assert.commandWorked(
                st.s.adminCommand({moveChunk: ns, find: {_id: 2}, to: st.shard1.shardName}),
            );
            setUpCollections(st.s);

            // The test database is touched only by this test: ShardingTest disables the balancer
            // and the setup moveChunk has committed, so no migration or resharding can register
            // entries for it after the reset.
            const nodes = [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()];
            nodes.forEach((conn) => resetQueryStatsStore(conn, "1MB"));
            runUserWorkload(st.s);
            assertNoInternalEntries(nodes);
        } finally {
            st.stop();
        }
    });
});

describe("query stats isInternal for a stamped operation", function () {
    it("is set on mongos and on the shard that mongos forwards the operation to", function () {
        const st = new ShardingTest({
            shards: 1,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });
        try {
            setUpCollections(st.s);
            const shardPrimary = st.rs0.getPrimary();
            [st.s, shardPrimary].forEach((conn) => resetQueryStatsStore(conn, "1MB"));

            const comment = "stamped";
            const fp = configureFailPoint(st.s, "stampIsInternalOperation", {comment});
            const coll = st.s.getDB(dbName)[collName];
            assert.eq(1, coll.find({_id: 1}).comment(comment).itcount());
            assert.eq(1, coll.find({_id: 2}).itcount());
            fp.off();

            for (const conn of [st.s, shardPrimary]) {
                const finds = getEntriesForTestDb(conn).filter(
                    (e) => e.key.queryShape.command === "find",
                );
                // $queryStats shapes the comment value, so only its presence identifies the find.
                const stamped = finds.filter((e) => e.key.hasOwnProperty("comment"));
                const unstamped = finds.filter((e) => !e.key.hasOwnProperty("comment"));
                assert.eq(1, stamped.length, {host: conn.host, finds});
                assert.eq(true, stamped[0].key.isInternal, {host: conn.host, entry: stamped[0]});
                assert.eq(1, unstamped.length, {host: conn.host, finds});
                assert(!unstamped[0].key.hasOwnProperty("isInternal"), {
                    host: conn.host,
                    entry: unstamped[0],
                });
            }
        } finally {
            st.stop();
        }
    });
});

describe("isInternalOperation generic argument", function () {
    const keyFile = "jstests/libs/key1";
    let st;

    function createUsers(conn) {
        const admin = conn.getDB("admin");
        admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        assert(admin.auth("admin", "pwd"));
        admin.createUser({user: "user", pwd: "pwd", roles: ["readWriteAnyDatabase"]});
        admin.logout();
    }

    before(function () {
        st = new ShardingTest({shards: 1, other: {keyFile}});
        createUsers(st.s);
        // In config shard mode the shard is the config server, which already has the users.
        if (!st.isConfigShardMode) {
            createUsers(st.rs0.getPrimary());
        }
    });

    after(function () {
        st?.stop();
    });

    it("is rejected from a client without internal authorization", function () {
        for (const host of [st.s.host, st.rs0.getPrimary().host]) {
            const conn = new Mongo(host);
            assert(conn.getDB("admin").auth("user", "pwd"));
            assert.commandFailedWithCode(
                conn.getDB(dbName).runCommand({find: collName, isInternalOperation: true}),
                12313100,
            );
        }
    });
});
