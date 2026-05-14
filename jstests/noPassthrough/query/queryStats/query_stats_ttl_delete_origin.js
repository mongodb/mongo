/**
 * SERVER-119359: $queryStats must label TTL-driven deletes distinctly from
 * user-driven deletes. A TTL pass and a peer user delete against the same
 * shape land in two separate query stats keys, distinguished by
 * `key.origin`.
 *
 * Pins:
 *   - DeleteKey is registered for both origins (rows exist).
 *   - `key.origin == "ttl"` for the TTL-monitor delete.
 *   - `key.origin == "client"` for the user delete.
 *   - No cross-contamination: each row's exec count is its own.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_90,
 *   uses_ttl,
 *   featureFlagQueryStatsDeleteCommand,
 * ]
 */
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const collName = jsTestName();

const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            ttlMonitorSleepSecs: 1,
            internalQueryStatsRateLimit: -1,
            internalQueryStatsCacheSize: "1MB",
        },
        slowms: 0,
    },
});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const testDB = primary.getDB(jsTestName());
const coll = testDB[collName];

// Reset store so we observe only this test's deletes.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));
coll.drop();

// TTL index, immediate expiry semantics.
assert.commandWorked(coll.createIndex({expireAt: 1}, {expireAfterSeconds: 0}));

// One expired doc (TTL monitor will reap) + one live doc (user will delete).
const past = new Date(Date.now() - 60 * 60 * 1000);
const future = new Date(Date.now() + 60 * 60 * 1000);
assert.commandWorked(coll.insert([
    {_id: "ttl-victim", expireAt: past, payload: 1},
    {_id: "user-victim", expireAt: future, payload: 2},
]));

// Wait for the TTL monitor to reap the expired doc.
assert.soon(
    () => coll.findOne({_id: "ttl-victim"}) === null,
    "TTL monitor did not reap expired doc within timeout",
    30 * 1000,
);

// Peer user delete, same shape (predicate on `_id`).
assert.commandWorked(testDB.runCommand({
    delete: collName,
    deletes: [{q: {_id: "user-victim"}, limit: 1}],
    comment: "user-driven delete for SERVER-119359 origin pinning",
}));

// Both targets gone — confirm physical state matches recorded stats.
assert.eq(0, coll.countDocuments({}), "both docs should be gone before reading queryStats");

// Read $queryStats — expect two `delete` rows on this collection, one per origin.
const deleteEntries = getQueryStats(primary, {collName: coll.getName()}).filter(
    (e) => e.key.queryShape && e.key.queryShape.command === "delete",
);

assert.gte(
    deleteEntries.length,
    2,
    "Expected at least 2 delete entries (ttl + client). Got: " + tojson(deleteEntries),
);

const byOrigin = {};
for (const e of deleteEntries) {
    const origin = e.key.origin;
    assert(
        origin === "ttl" || origin === "client" || origin === "internal",
        "Unexpected origin label on delete key: " + tojson(e.key),
    );
    byOrigin[origin] = (byOrigin[origin] || []).concat([e]);
}

assert(byOrigin.ttl && byOrigin.ttl.length >= 1, "Missing TTL-origin delete entry: " + tojson(deleteEntries));
assert(byOrigin.client && byOrigin.client.length >= 1, "Missing client-origin delete entry: " + tojson(deleteEntries));

// No cross-contamination: each row's exec count is its own bucket.
const ttlExec = byOrigin.ttl[0].metrics.execCount.sum;
const clientExec = byOrigin.client[0].metrics.execCount.sum;
assert.gte(ttlExec, 1, "TTL delete exec count should be >= 1: " + tojson(byOrigin.ttl[0]));
assert.gte(clientExec, 1, "Client delete exec count should be >= 1: " + tojson(byOrigin.client[0]));

// Shape parity — both rows describe deletes against the same namespace.
assert.eq(
    byOrigin.ttl[0].key.queryShape.cmdNs.coll,
    byOrigin.client[0].key.queryShape.cmdNs.coll,
    "ttl + client delete keys should share collection name; only origin differs",
);

jsTest.log("SERVER-119359: ttl vs client delete origins observed distinctly in $queryStats");

replTest.stopSet();
