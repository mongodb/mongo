/**
 * Verify TTL expiration on a clustered collection (clusteredIndex + expireAfterSeconds) in a
 * standalone, exercising the collection-scan TTL code path distinct from index-scan TTL covered
 * elsewhere. Confirms: (a) expired docs are reaped, (b) non-expired docs are retained,
 * (c) collMod-adjusted expireAfterSeconds takes effect at runtime, and (d) ttl.passes advances
 * across the deletions.
 *
 * SERVER-96179: extends TTL unit testing into the clustered-collection collection-scan path.
 */
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const conn = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
const db = conn.getDB("test");
const collName = "ttl_clustered";

db[collName].drop();
assert.commandWorked(
    db.createCollection(collName, {
        clusteredIndex: {key: {_id: 1}, unique: true},
        expireAfterSeconds: 10,
    }),
);
const coll = db[collName];

const now = new Date();
const expiredTs = new Date(now.getTime() - 3600 * 1000 * 24); // 24h old
const freshTs = new Date(now.getTime() + 3600 * 1000); // 1h in the future

// Mix expired and not-yet-expired documents. The clustered TTL path keys on _id, which must be a
// Date for expiration to apply.
for (let i = 0; i < 30; i++) {
    assert.commandWorked(coll.insert({_id: new Date(expiredTs.getTime() + i), payload: i}));
}
for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({_id: new Date(freshTs.getTime() + i), payload: 1000 + i}));
}
assert.eq(40, coll.find().itcount(), "all clustered docs should be present pre-expiry");

// Wait for the TTL monitor to reap the 30 expired docs; the 10 future-dated docs must remain.
assert.soon(function () {
    return coll.find().itcount() == 10;
}, "Clustered TTL should reap expired docs and retain future-dated docs");

// Confirm survivors are exactly the future-dated docs.
const survivors = coll.find({}, {payload: 1}).toArray();
for (const doc of survivors) {
    assert.gte(doc.payload, 1000, "Only future-dated payloads should survive: " + tojson(doc));
}

// collMod to a much larger expireAfterSeconds — the previously-future docs are still in-window
// and must remain. Insert a fresh batch of slightly-old docs that are within the new window.
assert.commandWorked(
    db.runCommand({collMod: collName, expireAfterSeconds: 3600 * 24 * 7}),
);

const recentTs = new Date(now.getTime() - 60 * 1000); // 1m old, inside 7-day window
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({_id: new Date(recentTs.getTime() + i), payload: 5000 + i}));
}

const passesBefore = db.serverStatus().metrics.ttl.passes;
TTLUtil.waitForPass(db);
assert.gt(
    db.serverStatus().metrics.ttl.passes,
    passesBefore,
    "ttl.passes must advance after collMod regardless of whether deletions occurred",
);
assert.eq(
    15,
    coll.find().itcount(),
    "5 fresh + 10 future docs must all remain inside the widened TTL window",
);

// Tighten the window so the recent-but-now-expired batch becomes reapable.
assert.commandWorked(db.runCommand({collMod: collName, expireAfterSeconds: 30}));
assert.soon(function () {
    return coll.find().itcount() == 10;
}, "After tightening TTL, recent docs should expire and only the future-dated 10 remain");

MongoRunner.stopMongod(conn);
