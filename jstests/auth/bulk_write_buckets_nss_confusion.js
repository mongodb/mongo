/*
 * Auth regression test: a bulkWrite must not let a user write a system.buckets namespace they are
 * not authorized for by listing it in nsInfo alongside a collection they can write.
 *
 * A user with write privileges only on "test.coll" (and none on "test.system.buckets.coll") issues a
 * bulkWrite whose ops reference "test.coll" but whose nsInfo also declares "test.system.buckets.coll".
 * Authorization only checks the referenced namespace, so the command is authorized against
 * "test.coll"; previously the sharded router then silently executed the write against
 * "test.system.buckets.coll", writing a system namespace excluded from the user's role. Verify the
 * write lands in "test.coll" and the system.buckets namespace is untouched.
 *
 * @tags: [
 *   requires_sharding,
 *   # The router-side guard asserted here does not exist in older binaries, so this test cannot
 *   # pass in multiversion suites that run a mixed-version cluster.
 *   requires_fcv_90,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
    other: {keyFile: "jstests/libs/key1"},
});

const kDbName = "test";
const kColl = "coll";
const kBucket = "system.buckets.coll";

const admin = st.s0.getDB("admin");
admin.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
assert(admin.auth("admin", "pass"));

// "coll" is an ordinary (non-timeseries) collection, sharded so it is tracked in the global catalog.
const testDb = st.s0.getDB(kDbName);
assert.commandWorked(
    st.s0.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(testDb.createCollection(kColl));
assert.commandWorked(st.s0.adminCommand({shardCollection: `${kDbName}.${kColl}`, key: {_id: 1}}));

// A role that grants writes ONLY on test.coll -- no privileges on test.system.buckets.coll or the
// system-buckets resource.
admin.createRole({
    role: "collOnlyWriter",
    privileges: [
        {resource: {db: kDbName, collection: kColl}, actions: ["insert", "update", "remove"]},
    ],
    roles: [],
});
admin.createUser({user: "collWriter", pwd: "pass", roles: ["collOnlyWriter"]});
admin.logout();

// Act as the limited user.
assert(admin.auth("collWriter", "pass"));

// Sanity: the user genuinely cannot write the system.buckets namespace directly.
assert.commandFailedWithCode(
    admin.runCommand({
        bulkWrite: 1,
        ops: [{insert: 0, document: {_id: 100, x: 1}}],
        nsInfo: [{ns: `${kDbName}.${kBucket}`}],
    }),
    [ErrorCodes.Unauthorized],
    "user unexpectedly authorized to write the system.buckets namespace directly",
);

// The confusion attempt: ops reference test.coll (index 0), but nsInfo also declares the buckets
// namespace. Authorized against test.coll, so the command succeeds.
const doc = {_id: 1, x: 42};
assert.commandWorked(
    admin.runCommand({
        bulkWrite: 1,
        ops: [{insert: 0, document: doc}],
        nsInfo: [{ns: `${kDbName}.${kColl}`}, {ns: `${kDbName}.${kBucket}`}],
    }),
);
admin.logout();

// As admin, confirm the write landed in the authorized collection and NOT in the system.buckets
// namespace the user was never authorized for.
assert(admin.auth("admin", "pass"));
assert.eq(
    1,
    testDb.getCollection(kColl).find({_id: 1}).itcount(),
    "insert did not land in test.coll",
);
// The system.buckets namespace must not have been created/written by the misrouted write. Direct
// find() on a system.buckets namespace is rejected by the server, so check via listCollections that
// the namespace does not exist at all.
assert.eq(
    0,
    testDb.getCollectionInfos({name: kBucket}).length,
    "insert was misrouted to the system.buckets namespace the user could not write",
);
admin.logout();

st.stop();
