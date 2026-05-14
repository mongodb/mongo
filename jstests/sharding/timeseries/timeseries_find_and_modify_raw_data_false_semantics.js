/**
 * Pins the semantics of the `rawData` argument on `findAndModify` against a sharded time-series
 * collection. `rawData: false` must be identical to omitting `rawData` entirely on BOTH:
 *
 *  1. the cluster-side routing path (`src/mongo/s/commands/query_cmd/cluster_find_and_modify_cmd.cpp`)
 *     — the request must NOT enter the raw-data execution path, so updates must operate on the
 *     user-facing namespace, not the underlying bucket documents.
 *
 *  2. the generic-argument authorization path (`checkAuthForRawData` in
 *     `src/mongo/db/commands.cpp`) — the request must NOT require the `performRawDataOperations`
 *     privilege, so a user with `findAndModify` on the collection but without
 *     `performRawDataOperations` must succeed.
 *
 * Regression test for SERVER-126411 (the routing path was using `getRawData().has_value()` after
 * the SERVER-119290 TypedCommand conversion in commit dab99de2; this caused `rawData: false` to
 * enter the raw-data execution path while skipping the privilege check, an authorization mismatch
 * also tracked in SECBUG-788 / HackerOne 3675562).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # findAndModify on time-series is not retryable in current master.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const st = new ShardingTest({shards: 2, keyFile: "jstests/libs/key1"});

const dbName = "testDB";
const collName = "ts";
const timeField = "time";
const metaField = "meta";

const adminDb = st.s.getDB("admin");

assert.commandWorked(adminDb.runCommand({createUser: "root", pwd: "pwd", roles: ["__system"]}));
assert(adminDb.auth("root", "pwd"));

// User A: findAndModify on the time-series collection, but NO `performRawDataOperations`.
// We grant `readWrite` on the test database, which carries `find` + `update` + `remove` on every
// non-system collection but does NOT grant `performRawDataOperations` (that action is restricted
// to the built-in roles that explicitly enumerate it — see jstests/auth/builtin_roles.js).
assert.commandWorked(
    adminDb.runCommand({
        createUser: "userNoRaw",
        pwd: "pwd",
        roles: [{role: "readWrite", db: dbName}],
    }),
);

// User B: same as User A plus a custom role that adds `performRawDataOperations` so we can
// observe the raw-data path on the SAME collection as a positive control.
assert.commandWorked(
    adminDb.runCommand({
        createRole: "rawDataOnTs",
        roles: [],
        privileges: [
            {
                resource: {db: dbName, collection: collName},
                actions: ["performRawDataOperations"],
            },
        ],
    }),
);
assert.commandWorked(
    adminDb.runCommand({
        createUser: "userRaw",
        pwd: "pwd",
        roles: [
            {role: "readWrite", db: dbName},
            {role: "rawDataOnTs", db: "admin"},
        ],
    }),
);

const mongosDB = st.s.getDB(dbName);

assert.commandWorked(
    mongosDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

const coll = mongosDB.getCollection(collName);
assert.commandWorked(coll.createIndex({[metaField]: 1}));
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {[metaField]: 1}}),
);

// Seed user-visible measurements. Each meta value goes to its own bucket; we split chunks across
// shards to force the request through the router.
const numDocs = 20;
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(coll.insert({[timeField]: ISODate("2026-05-14T00:00:00Z"), [metaField]: i, payload: "v0"}));
}
const bucketsNs = getTimeseriesCollForDDLOps(mongosDB, coll).getFullName();
assert.commandWorked(mongosDB.adminCommand({split: bucketsNs, middle: {[metaField]: 10}}));
assert.commandWorked(
    mongosDB.adminCommand({moveChunk: bucketsNs, find: {[metaField]: 15}, to: st.shard1.shardName, _waitForDelete: true}),
);

adminDb.logout();

// ---------------------------------------------------------------------------
// Routing path: `rawData: false` must NOT enter the raw-data execution path.
// We verify this by running `findAndModify` with `rawData: false` as a user that does NOT hold
// `performRawDataOperations`. If the router (incorrectly) routed this through the raw-data
// execution path, the generic-argument auth check in `commands.cpp` would reject the request with
// `Unauthorized`, OR (worse) the request would silently mutate a `system.buckets.<coll>` bucket
// document rather than the user-facing measurement. We assert: (a) the command succeeds, and
// (b) the mutation lands on the user-facing measurement, not on a bucket field.
// ---------------------------------------------------------------------------

assert(mongosDB.auth("userNoRaw", "pwd"));

// (a) Command succeeds with `rawData: false` despite the user lacking `performRawDataOperations`.
const targetMeta = 3;
const famFalse = assert.commandWorked(
    mongosDB.runCommand({
        findAndModify: collName,
        query: {[metaField]: targetMeta},
        update: {$set: {payload: "via_rawData_false"}},
        rawData: false,
    }),
);
assert.eq(1, famFalse.lastErrorObject.n, () => `rawData:false update should affect 1 measurement: ${tojson(famFalse)}`);

// (b) The mutation landed on a user-visible measurement. If `rawData: false` had been interpreted
// as raw-data, the update path would have attempted to set `payload` on a bucket control document
// rather than on the unpacked measurement, which would either fail or land in the wrong place.
const updated = coll.find({[metaField]: targetMeta}).toArray();
assert.eq(1, updated.length, () => `expected one measurement back; got ${tojson(updated)}`);
assert.eq("via_rawData_false", updated[0].payload, () => `unexpected payload: ${tojson(updated[0])}`);

// `rawData` omitted entirely must produce identical behavior (this is the contract: false ≡ unset).
const famOmitted = assert.commandWorked(
    mongosDB.runCommand({
        findAndModify: collName,
        query: {[metaField]: targetMeta},
        update: {$set: {payload: "via_rawData_omitted"}},
    }),
);
assert.eq(1, famOmitted.lastErrorObject.n);
assert.eq("via_rawData_omitted", coll.findOne({[metaField]: targetMeta}).payload);

mongosDB.logout();

// ---------------------------------------------------------------------------
// Authorization path: `rawData: true` MUST require `performRawDataOperations`. The same user
// (no `performRawDataOperations`) must be rejected with `Unauthorized` when the field is true.
// This pins the other half of the contract — only `true` is raw-data; the auth gate fires only
// for `true`.
// ---------------------------------------------------------------------------

assert(mongosDB.auth("userNoRaw", "pwd"));

assert.commandFailedWithCode(
    mongosDB.runCommand({
        findAndModify: collName,
        query: {[metaField]: targetMeta},
        update: {$set: {payload: "via_rawData_true"}},
        rawData: true,
    }),
    ErrorCodes.Unauthorized,
    "rawData:true on a user without performRawDataOperations must be rejected",
);

mongosDB.logout();

// Positive control: the privileged user CAN run `findAndModify` with `rawData: true`. This proves
// the auth gate is wired correctly (it isn't accidentally rejecting everyone, which would
// falsely make the negative assertion above pass).
assert(mongosDB.auth("userRaw", "pwd"));

const famTrue = assert.commandWorked(
    mongosDB.runCommand({
        findAndModify: collName,
        query: {"meta": targetMeta},
        update: {$set: {"control.version": NumberInt(2)}},
        rawData: true,
    }),
    "rawData:true must succeed for a user holding performRawDataOperations",
);
assert.eq(1, famTrue.lastErrorObject.n, () => tojson(famTrue));

mongosDB.logout();

st.stop();
