import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test because AuthoritativeShards is already enabled in lastLTS");
    quit();
}

// Start on a fully non-authoritative shard (lastLTS FCV).
const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB("test");
const coll = db.foo;
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
);
assert.commandWorked(db.runCommand({create: coll.getName()}));

function assertCatalogEntries(shard, nExpected) {
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

// Hang a non-authoritative dropCollection after it decides to be non-authoritative,
// but before it grabs the DDL lock.
const fpDrop = configureFailPoint(st.rs0.getPrimary(), "hangNonAuthoritativeDDLBeforeDDLLock");
const dropThread = new Thread(
    (mongosHost, dbName, collName) => {
        return new Mongo(mongosHost).getDB(dbName).runCommand({drop: collName});
    },
    st.s.host,
    db.getName(),
    coll.getName(),
);
dropThread.start();
fpDrop.wait();

// Fail setFCV after the DB primary shard is already in kUpgrading (DDLs commit authoritatively).
configureFailPoint(st.rs0.getPrimary(), "failAfterReachingTransitioningState", {}, {times: 1});
assert.commandFailed(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Authoritatively shard a collection. The {_id: "hashed"} shard key causes chunks to be
// distributed evenly across shards, so both s0 and s1 get a collection entry.
assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
);
assertCatalogEntries(st.shard0, 1);
assertCatalogEntries(st.shard1, 1);

// Resume the drop. If it were non-authoritative it would leave s0 and s1's collection entries.
// However, it's transparently retried authoritatively, which will delete both entries.
fpDrop.off();
dropThread.join();
assert.commandWorked(dropThread.returnData());

assertCatalogEntries(st.shard0, 0);
assertCatalogEntries(st.shard1, 0);

st.stop();
