/**
 * Tests that expired unowned documents (orphans) on a shard do not stop the TTL monitor from
 * deleting the owned expired documents that come after them. Each case leaves more orphans than
 * 'ttlIndexDeleteTargetDocs' on shard0, so a TTL pass only gets past them by resuming each batched
 * delete after the last document the previous one staged.
 *
 * @tags: [requires_fcv_91]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {runTimeseriesChunkCommand} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

// The range deleter is disabled so the orphans stay; the shutdown orphan check would never pass.
TestData.skipCheckOrphans = true;

const kTargetDocs = 100;
const kOrphans = 500;
const kOwned = 50;
const kIndexResumeLogId = 9277901;
const kClusteredResumeLogId = 9277902;

const range = (n, f) => Array.from({length: n}, (_, i) => f(i + 1));

describe("TTL deletes past expired unowned documents", function () {
    let st;
    let db;
    let shard0;
    const dbName = jsTest.name();

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {
                nodes: 1,
                setParameter: {
                    ttlMonitorSleepSecs: 1,
                    disableResumableRangeDeleter: true,
                    logComponentVerbosity: tojson({index: 2}),
                },
            },
        });
        db = st.s.getDB(dbName);
        shard0 = st.rs0.getPrimary();
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(
            shard0.adminCommand({setParameter: 1, ttlIndexDeleteTargetDocs: kTargetDocs}),
        );
    });

    after(function () {
        st.stop();
    });

    // Inserts 'orphans' and 'owned' into a new collection sharded on 'shardKey', all on shard0,
    // then moves the chunk at 'orphanChunk' to shard1 so the orphans stay behind as unowned
    // documents. 'makeTTL' creates the TTL index (or sets expireAfterSeconds) afterwards so no
    // TTL delete runs before the orphans exist.
    function setUpCollection({
        collName,
        shardKey,
        unique,
        split,
        orphanChunk,
        orphans,
        owned,
        makeTTL,
        create,
    }) {
        const ns = dbName + "." + collName;
        if (create) {
            create(collName);
        }
        assert.commandWorked(
            st.s.adminCommand({shardCollection: ns, key: shardKey, unique: !!unique}),
        );

        const docs = [];
        orphans.forEach((doc) => docs.push({...doc, kind: "orphan"}));
        owned.forEach((doc) => docs.push({...doc, kind: "owned"}));
        assert.commandWorked(db[collName].insert(docs));

        assert.commandWorked(st.s.adminCommand({split: ns, middle: split}));
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: ns,
                find: orphanChunk,
                to: st.shard1.shardName,
                _waitForDelete: false,
            }),
        );

        const shardColl = shard0.getDB(dbName)[collName];
        assert.eq(orphans.length, shardColl.countDocuments({kind: "orphan"}), "orphans on shard0");
        assert.eq(owned.length, shardColl.countDocuments({kind: "owned"}), "owned on shard0");

        makeTTL(collName);
        return shardColl;
    }

    function assertOwnedDeletedOrphansKept(shardColl, nOrphans) {
        assert.soon(
            () => shardColl.countDocuments({kind: "owned"}) === 0,
            "owned expired documents were not deleted",
            60 * 1000,
        );
        assert.eq(nOrphans, shardColl.countDocuments({kind: "orphan"}), "TTL deleted orphans");
    }

    function assertResumed(logId, collName) {
        assert(
            checkLog.checkContainsOnceJson(shard0, logId, {namespace: dbName + "." + collName}),
            "TTL did not resume a batched delete",
            {logId, collName},
        );
    }

    const ttlIndex =
        (key, options = {}) =>
        (collName) =>
            assert.commandWorked(
                db[collName].createIndex({t: key}, {expireAfterSeconds: 0, ...options}),
            );

    // Orphans hold the oldest dates; owned documents come after them in index order.
    const orphansFirst = {
        shardKey: {sk: 1},
        split: {sk: 0},
        orphanChunk: {sk: 1},
        orphans: range(kOrphans, (i) => ({sk: i, t: new Date(1000 + i)})),
        owned: range(kOwned, (i) => ({sk: -i, t: new Date(100000 + i)})),
    };

    it("ascending TTL index", function () {
        const collName = "ascending";
        const shardColl = setUpCollection({collName, ...orphansFirst, makeTTL: ttlIndex(1)});
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("descending TTL index", function () {
        const collName = "descending";
        const shardColl = setUpCollection({collName, ...orphansFirst, makeTTL: ttlIndex(-1)});
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("orphans interleaved with owned documents", function () {
        const collName = "interleaved";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            orphans: range(kOrphans, (i) => ({sk: i, t: new Date(1000 + 2 * i)})),
            owned: range(kOrphans, (i) => ({sk: -i, t: new Date(1001 + 2 * i)})),
            makeTTL: ttlIndex(1),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("orphans sharing one expiry date", function () {
        // Restarting at the shared key would stage the same orphans forever, so the next delete
        // starts after that key.
        const collName = "tiedOrphans";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            orphans: range(kOrphans, (i) => ({sk: i, t: new Date(5000)})),
            makeTTL: ttlIndex(1),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assert(
            checkLog.checkContainsOnceJson(shard0, kIndexResumeLogId, {
                namespace: dbName + "." + collName,
                excludeResumeKey: true,
            }),
            "TTL did not start after the shared key",
        );
        assert(db[collName].drop());
    });

    it("multikey TTL index", function () {
        // Each orphan has a second, later date, so orphans appear both before and after the owned
        // documents in the index.
        const collName = "multikey";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            orphans: range(kOrphans, (i) => ({
                sk: i,
                t: [new Date(1000 + i), new Date(200000 + i)],
            })),
            makeTTL: ttlIndex(1),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("unique TTL index on the shard key", function () {
        // A unique index must be prefixed by the shard key, so shard on the TTL field, move the
        // oldest range away, then make the unique shard key index a TTL index.
        const collName = "uniqueShardKey";
        const shardColl = setUpCollection({
            collName,
            shardKey: {t: 1},
            unique: true,
            split: {t: new Date(50000)},
            orphanChunk: {t: new Date(1000)},
            orphans: range(kOrphans, (i) => ({t: new Date(1000 + i)})),
            owned: range(kOwned, (i) => ({t: new Date(100000 + i)})),
            makeTTL: (name) =>
                assert.commandWorked(
                    db.runCommand({
                        collMod: name,
                        index: {keyPattern: {t: 1}, expireAfterSeconds: 0},
                    }),
                ),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("partial TTL index", function () {
        const collName = "partial";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            makeTTL: ttlIndex(1, {partialFilterExpression: {sk: {$exists: true}}}),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kIndexResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("clustered collection", function () {
        // TTL scans the clustered _id and resumes after the last staged RecordId.
        const collName = "clustered";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            orphans: range(kOrphans, (i) => ({sk: i, _id: new Date(1000 + i)})),
            owned: range(kOwned, (i) => ({sk: -i, _id: new Date(100000 + i)})),
            create: (name) =>
                assert.commandWorked(
                    db.createCollection(name, {clusteredIndex: {key: {_id: 1}, unique: true}}),
                ),
            makeTTL: (name) =>
                assert.commandWorked(db.runCommand({collMod: name, expireAfterSeconds: 0})),
        });
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kClusteredResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("time-series collection with pre-1970 dates", function () {
        // Pre-1970 dates force the extended-range path, which deletes those buckets with a backward
        // clustered scan starting nearest to 1970. One bucket per measurement since each has its
        // own meta value; orphans are nearer 1970 so the scan meets them first.
        const collName = "timeseries";
        const ns = dbName + "." + collName;
        assert.commandWorked(
            db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}),
        );
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {m: 1}}));

        const docs = [];
        range(kOrphans, (i) => docs.push({m: i, t: new Date(-(1000 + i) * 1000), kind: "orphan"}));
        range(kOwned, (i) => docs.push({m: -i, t: new Date(-(1000000 + i) * 1000), kind: "owned"}));
        assert.commandWorked(db[collName].insert(docs));

        runTimeseriesChunkCommand(db, {split: ns, middle: {meta: 0}});
        runTimeseriesChunkCommand(db, {
            moveChunk: ns,
            find: {meta: 1},
            to: st.shard1.shardName,
            _waitForDelete: false,
        });

        const shardColl = shard0.getDB(dbName)[collName];
        assert.eq(kOrphans, shardColl.countDocuments({kind: "orphan"}), "orphans on shard0");
        assert.eq(kOwned, shardColl.countDocuments({kind: "owned"}), "owned on shard0");

        assert.commandWorked(db.runCommand({collMod: collName, expireAfterSeconds: 0}));
        assertOwnedDeletedOrphansKept(shardColl, kOrphans);
        assertResumed(kClusteredResumeLogId, collName);
        assert(db[collName].drop());
    });

    it("owned documents sharing the orphans' expiry date are deferred", function () {
        // TODO(SERVER-138186): expect the owned documents to be deleted once a batched delete can
        // resume at a (key, RecordId) position.
        const collName = "tiedOwned";
        const shardColl = setUpCollection({
            collName,
            ...orphansFirst,
            orphans: range(kOrphans, (i) => ({sk: i, t: new Date(5000)})),
            owned: range(10, (i) => ({sk: -i, t: new Date(5000)})),
            makeTTL: ttlIndex(1),
        });
        const passes = () => shard0.adminCommand({serverStatus: 1}).metrics.ttl.passes;
        const startPasses = passes();
        assert.soon(() => passes() >= startPasses + 3, "TTL passes stopped completing");
        assert.eq(
            10,
            shardColl.countDocuments({kind: "owned"}),
            "owned documents with the tied key",
        );
        assert.eq(kOrphans, shardColl.countDocuments({kind: "orphan"}), "TTL deleted orphans");
        assert(db[collName].drop());
    });
});
