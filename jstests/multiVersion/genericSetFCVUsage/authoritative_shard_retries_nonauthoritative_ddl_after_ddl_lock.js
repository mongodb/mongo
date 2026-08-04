/**
 * Tests that non-authoritative DDL that begins executing after an FCV upgrade is retried
 * authoritatively.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test because AuthoritativeShards is already enabled in lastLTS");
    quit();
}

describe("authoritative shard retries non-authoritative DDL", () => {
    let st;
    let db;

    before(() => {
        st = new ShardingTest({shards: 2, config: 1});
    });

    after(() => {
        st.stop();
    });

    beforeEach(() => {
        db = st.s.getDB("test");
        assert.commandWorked(db.dropDatabase());
        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        assert.commandWorked(
            st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    function assertCatalogEntries(shard, coll, nExpected) {
        const entries = shard
            .getDB("config")
            .shard.catalog.collections.find({_id: coll.getFullName()})
            .toArray();
        assert.eq(
            nExpected,
            entries.length,
            `Unexpected number of shard catalog entries in ${shard.shardName}`,
            {entries},
        );
    }

    function runTestCase(command, whileNonAuthoritativeIsHungFn) {
        // Hang a non-authoritative command after it decides to be non-authoritative, but before it
        // grabs the DDL lock.
        const failpoint = configureFailPoint(
            st.rs0.getPrimary(),
            "hangNonAuthoritativeDDLBeforeDDLLock",
        );
        const commandThread = new Thread(
            (mongosHost, dbName, command) => {
                return new Mongo(mongosHost).getDB(dbName).runCommand(command);
            },
            st.s.host,
            db.getName(),
            command,
        );
        commandThread.start();
        failpoint.wait();

        // Fail setFCV after the DB primary shard is already in kUpgrading (DDLs commit
        // authoritatively).
        configureFailPoint(
            st.rs0.getPrimary(),
            "failAfterReachingTransitioningState",
            {},
            {times: 1},
        );
        assert.commandFailed(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );

        whileNonAuthoritativeIsHungFn();

        // Resume the command. It should succeed authoritatively.
        failpoint.off();
        commandThread.join();
        assert.commandWorked(commandThread.returnData());
    }

    it("retries dropCollection authoritatively", () => {
        const coll = db.foo;
        assert.commandWorked(db.runCommand({create: coll.getName()}));

        runTestCase({drop: coll.getName()}, () => {
            // Authoritatively shard a collection. The {_id: "hashed"} shard key causes chunks to be
            // distributed evenly across shards, so both s0 and s1 get a collection entry.
            assert.commandWorked(
                st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
            );
            assertCatalogEntries(st.shard0, coll, 1);
            assertCatalogEntries(st.shard1, coll, 1);
        });

        // Assert that the drop cleaned up the collection entries. If it were non-authoritative it
        // would leave s0 and s1's collection entries. However, it's transparently retried
        // authoritatively, which will delete both entries.
        assertCatalogEntries(st.shard0, coll, 0);
        assertCatalogEntries(st.shard1, coll, 0);
    });

    it("retries $out rename authoritatively", () => {
        const srcColl = db.src;
        const outColl = db.out;
        const kNumDocs = 10;

        assert.commandWorked(
            srcColl.insertMany([...Array(kNumDocs).keys()].map((i) => ({_id: i}))),
        );

        runTestCase(
            {
                aggregate: srcColl.getName(),
                pipeline: [{$out: outColl.getName()}],
                cursor: {},
            },
            () => {},
        );

        // The rename executed by $out cannot be retried by the service entry point, but it has its
        // own retry logic to handle the FCV transition. This ensures that the operation will be
        // transparently retried authoritatively.
        assert.eq(
            kNumDocs,
            outColl.find().itcount(),
            "Unexpected number of documents in $out target",
        );
    });
});
