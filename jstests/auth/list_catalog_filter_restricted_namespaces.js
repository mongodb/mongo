/**
 * Tests that collectionless $listCatalog hides config.*, local.*, and <any-database>.system.*
 * entries from non-internal callers while internal callers see the full catalog. Covers both
 * standalone (ReplSetTest) and sharded (ShardingTest) topologies, and both FCV 9.0 (viewless
 * timeseries) and FCV 8.0 (legacy timeseries with system.buckets.* catalog entries).
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_auth,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Create an internal-privileged user and a non-internal readAnyDatabase user.
// The __system role grants ActionType::internal, which bypasses the namespace filter.
function setupUsers(conn) {
    const adminDb = conn.getDB("admin");
    assert.commandWorked(
        adminDb.runCommand({createUser: "root", pwd: "rootpwd", roles: ["__system"]}),
    );
    assert(adminDb.auth("root", "rootpwd"));
    assert.commandWorked(
        adminDb.runCommand({createUser: "reader", pwd: "readerpwd", roles: ["readAnyDatabase"]}),
    );
    adminDb.logout();
}

/**
 * Sets up test fixtures, asserts filter behavior, then tears down fixtures.
 * Users are not created or dropped here — call setupUsers() once per cluster before invoking.
 *
 * @param {Mongo} conn   Mongo connection (mongos or replica set primary).
 */
function runFilterTest(conn) {
    const adminDb = conn.getDB("admin");

    // --- Setup ---

    assert(adminDb.auth("root", "rootpwd"));

    assert.commandWorked(conn.getDB("testdb").createCollection("usercoll"));
    // Create a view so testdb.system.views exists in the catalog.
    assert.commandWorked(conn.getDB("testdb").createView("myview", "usercoll", []));

    // Insert into testdb.system.js to exercise the system.js carve-out.
    assert.commandWorked(
        conn.getDB("testdb").runCommand({
            insert: "system.js",
            documents: [{_id: "testfn", value: "function(){}"}],
        }),
    );

    // Always create a timeseries collection. Under FCV < 9.0 (legacy timeseries) it produces
    // system.buckets.* catalog which is tested below.
    // TODO(SERVER-129907): remove the FCV downgrade block in the outer scope once 9.0 becomes
    // lastLTS — at that point system.buckets.* entries never appear.
    assert.commandWorked(
        conn.getDB("testdb").createCollection("tscoll", {timeseries: {timeField: "ts"}}),
    );

    adminDb.logout();

    // --- Assertions ---

    function listCatalogAs(user, pwd) {
        assert(adminDb.auth(user, pwd));
        const entries = adminDb.aggregate([{$listCatalog: {}}]).toArray();
        adminDb.logout();
        return entries;
    }

    const readerEntries = listCatalogAs("reader", "readerpwd");
    const rootEntries = listCatalogAs("root", "rootpwd");

    // config.* must be hidden from non-internal callers.
    const configEntries = readerEntries.filter((e) => e.db === "config");
    assert.eq(configEntries.length, 0, "reader should not see config.* entries", {configEntries});

    // local.* must be hidden from non-internal callers.
    const localEntries = readerEntries.filter((e) => e.db === "local");
    assert.eq(localEntries.length, 0, "reader should not see local.* entries", {localEntries});

    // Any <database>.system.* (other than the explicit carve-outs) must be hidden.
    const systemEntries = readerEntries.filter(
        (e) =>
            e.name.startsWith("system.") &&
            e.name !== "system.js" &&
            !e.name.startsWith("system.buckets."),
    );
    assert.eq(
        systemEntries.length,
        0,
        "reader should not see <database>.system.* entries (other than system.js / system.buckets.*)",
        {systemEntries},
    );

    // testdb.system.js must be visible (explicit carve-out, db-agnostic).
    const hasSystemJs = readerEntries.some((e) => e.db === "testdb" && e.name === "system.js");
    assert(hasSystemJs, "reader should see testdb.system.js", {readerEntries});

    // If system.buckets.* entries exist in the catalog (legacy timeseries, FCV < 9.0), they must
    // be visible to the reader via the explicit carve-out.
    // TODO(SERVER-129907): this branch will never be taken once 9.0 becomes lastLTS.
    if (rootEntries.some((e) => e.name.startsWith("system.buckets."))) {
        const hasSystemBuckets = readerEntries.some((e) => e.name.startsWith("system.buckets."));
        assert(hasSystemBuckets, "reader should see system.buckets.*", {readerEntries});
    }

    // Normal user collections must be visible.
    const hasUserColl = readerEntries.some((e) => e.db === "testdb" && e.name === "usercoll");
    assert(hasUserColl, "reader should see testdb.usercoll", {readerEntries});

    // Internal caller must see config.* (config.system.indexBuilds is always created on step-up).
    const rootConfigEntries = rootEntries.filter((e) => e.db === "config");
    assert(rootConfigEntries.length > 0, "root should see config.* entries", {rootEntries});

    // Internal caller must see local.* (local.startup_log is always present).
    const rootLocalEntries = rootEntries.filter((e) => e.db === "local");
    assert(rootLocalEntries.length > 0, "root should see local.* entries", {rootEntries});

    // Internal caller must see testdb.system.views (created via createView above).
    const hasSystemViews = rootEntries.some((e) => e.db === "testdb" && e.name === "system.views");
    assert(hasSystemViews, "root should see testdb.system.views", {rootEntries});

    // Internal caller sees a superset of what non-internal caller sees.
    const readerNs = new Set(readerEntries.map((e) => e.ns));
    const rootNs = new Set(rootEntries.map((e) => e.ns));
    for (const ns of readerNs) {
        assert(rootNs.has(ns), "root should include all namespaces visible to reader", {ns});
    }

    // Verify that every namespace visible to the reader is actually readable, confirming no
    // namespace was leaked without proper access.
    assert(adminDb.auth("reader", "readerpwd"));
    for (const entry of readerEntries) {
        assert.commandWorked(
            conn.getDB(entry.db).runCommand({find: entry.name, limit: 0}),
            `reader should be able to find on ${entry.ns}`,
        );
    }
    adminDb.logout();

    // --- Teardown ---

    // Dropping testdb also removes system.js, system.views, and any timeseries collections.
    assert(adminDb.auth("root", "rootpwd"));
    conn.getDB("testdb").dropDatabase();
    adminDb.logout();
}

// --- ReplSetTest: 1-node replica set with keyFile auth ---
{
    const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    setupUsers(primary);

    runFilterTest(primary); // FCV 9.0: viewless timeseries

    // TODO(SERVER-129907): remove this FCV downgrade once 9.0 becomes lastLTS — at that point
    // viewless timeseries is always active and system.buckets.* entries never appear.
    assert(primary.getDB("admin").auth("root", "rootpwd"));
    assert.commandWorked(
        primary
            .getDB("admin")
            .runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    primary.getDB("admin").logout();

    runFilterTest(primary); // FCV lastLTS: legacy timeseries, system.buckets.* exercised

    rst.stopSet();
}

// --- ShardingTest: 1 shard with keyFile auth ---
{
    const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
    const mongos = st.s;

    setupUsers(mongos);

    runFilterTest(mongos); // FCV 9.0

    // TODO(SERVER-129907): remove this FCV downgrade once 9.0 becomes lastLTS — at that point
    // viewless timeseries is always active and system.buckets.* entries never appear.
    assert(mongos.getDB("admin").auth("root", "rootpwd"));
    assert.commandWorked(
        mongos
            .getDB("admin")
            .runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    mongos.getDB("admin").logout();

    runFilterTest(mongos); // FCV lastLTS

    st.stop();
}
