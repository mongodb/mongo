/**
 * Tests that collectionless $listCatalog hides config.*, local.*, and <any-database>.system.*
 * entries from non-internal callers while internal callers see the full catalog. Covers both
 * replica set (ReplSetTest) and sharded (ShardingTest) topologies.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_auth,
 * ]
 */

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

    // A collection whose name starts with "system" but is not a "system." namespace.
    assert.commandWorked(conn.getDB("testdb").createCollection("system_foo"));

    // A user collection resembling the system.buckets.* carve-out but not a "system." namespace.
    assert.commandWorked(conn.getDB("testdb").createCollection("system_buckets_foo"));

    // Create a timeseries collection to produce system.buckets.* catalog entries, which are tested
    // below.
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
    assert.eq(
        configEntries.length, 0, "reader should not see config.* entries: " + tojson(configEntries));

    // local.* must be hidden from non-internal callers.
    const localEntries = readerEntries.filter((e) => e.db === "local");
    assert.eq(
        localEntries.length, 0, "reader should not see local.* entries: " + tojson(localEntries));

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
        "reader should not see <database>.system.* entries (other than system.js / system.buckets.*): " +
            tojson(systemEntries),
    );

    // testdb.system.js must be visible (explicit carve-out, db-agnostic).
    const hasSystemJs = readerEntries.some((e) => e.db === "testdb" && e.name === "system.js");
    assert(hasSystemJs, "reader should see testdb.system.js: " + tojson(readerEntries));

    // A "system"-prefixed but non-"system." collection is a user collection and must be visible.
    const hasUserSystemFoo = readerEntries.some((e) => e.db === "testdb" && e.name === "system_foo");
    assert(hasUserSystemFoo, "reader should see testdb.system_foo: " + tojson(readerEntries));

    const hasUserSystemBuckets = readerEntries.some((e) => e.db === "testdb" && e.name === "system_buckets_foo");
    assert(hasUserSystemBuckets, "reader should see testdb.system_buckets_foo: " + tojson(readerEntries));

    // Verify that system.buckets.* entries are visible to the reader via the explicit carve-out.
    const hasSystemBuckets = readerEntries.some((e) => e.name.startsWith("system.buckets."));
    assert(hasSystemBuckets, "reader should see system.buckets.*: " + tojson(readerEntries));

    // Normal user collections must be visible.
    const hasUserColl = readerEntries.some((e) => e.db === "testdb" && e.name === "usercoll");
    assert(hasUserColl, "reader should see testdb.usercoll: " + tojson(readerEntries));

    // Internal caller must see config.* (config.system.indexBuilds is always created on step-up).
    const rootConfigEntries = rootEntries.filter((e) => e.db === "config");
    assert(rootConfigEntries.length > 0, "root should see config.* entries: " + tojson(rootEntries));

    // Internal caller must see local.* (local.startup_log is always present).
    const rootLocalEntries = rootEntries.filter((e) => e.db === "local");
    assert(rootLocalEntries.length > 0, "root should see local.* entries: " + tojson(rootEntries));

    // Internal caller must see testdb.system.views (created via createView above).
    const hasSystemViews = rootEntries.some((e) => e.db === "testdb" && e.name === "system.views");
    assert(hasSystemViews, "root should see testdb.system.views: " + tojson(rootEntries));

    // Internal caller sees a superset of what non-internal caller sees.
    const readerNs = new Set(readerEntries.map((e) => e.ns));
    const rootNs = new Set(rootEntries.map((e) => e.ns));
    for (const ns of readerNs) {
        assert(rootNs.has(ns), "root should include all namespaces visible to reader, missing: " + ns);
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

    runFilterTest(primary);

    rst.stopSet();
}

// --- ShardingTest: 1 shard with keyFile auth ---
{
    const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
    const mongos = st.s;

    setupUsers(mongos);

    runFilterTest(mongos);

    st.stop();
}
