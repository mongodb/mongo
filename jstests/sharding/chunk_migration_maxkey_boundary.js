/**
 * SERVER-121533: Verifies that documents with MaxKey shard key values are correctly migrated
 * during chunk migration and properly cleaned up by the range deleter.
 *
 * Two bugs caused MaxKey documents to be missed during migration:
 *
 * 1) extendRangeBound pads trailing index fields with MinKey for exclusive upper bounds. When the
 *    shard key index is wider than the shard key (e.g. shard key {x:1}, index {x:1,y:1}), this
 *    produces {x: MaxKey, y: MinKey} instead of {x: MaxKey, y: MaxKey}, excluding MaxKey docs
 *    from the scan range.
 *
 * 2) Index scans use an exclusive upper bound (kIncludeStartKeyOnly), so documents whose shard
 *    key is exactly MaxKey sit at the boundary and are excluded. This conflicts with isKeyInRange
 *    which treats MaxKey as inclusive (SERVER-67529), causing mongos to route to the destination
 *    shard while the cloner never copies the documents.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   requires_sharding,
 *   requires_fcv_83,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const testName = jsTestName();

function setupCollection(suffix, shardKey) {
    const dbName = testName + "_" + suffix;
    const ns = dbName + ".coll";
    const testDB = st.s.getDB(dbName);
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
    return {dbName, ns, coll: testDB.coll, shard0DB: st.shard0.getDB(dbName)};
}

function moveChunkAndWait(ns, findDoc) {
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: findDoc, to: st.shard1.shardName, _waitForDelete: true}),
    );
}

function assertMigrationComplete(dbName, ns, coll, shard0DB, expectedCount) {
    assert.eq(
        expectedCount,
        st.shard1.getDB(dbName).coll.find().itcount(),
        "Expected " + expectedCount + " docs on shard1",
    );
    assert.eq(expectedCount, coll.find().itcount(), "All docs visible via mongos");
    assert.eq(0, shard0DB.coll.find().itcount(), "shard0 should be empty after range deletion");
}

// Test 1: Exact shard key index — exclusive upper bound fix.
// With an exact index, the scan's exclusive upper bound excludes docs at exactly MaxKey.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("exact_idx", {x: 1});
    assert.commandWorked(coll.insertMany([{x: MaxKey()}, {x: 42}]));
    moveChunkAndWait(ns, {x: 0});
    assertMigrationComplete(dbName, ns, coll, shard0DB, 2);
    coll.drop();
}

// Test 2: Wider index — extendRangeBound MinKey padding fix.
// Shard key {x:1} with index {x:1,y:1}: trailing field padded with MinKey excludes MaxKey docs.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("wider_idx", {x: 1});
    assert.commandWorked(coll.createIndex({x: 1, y: 1}));
    assert.commandWorked(coll.dropIndex({x: 1}));
    assert.commandWorked(
        coll.insertMany([
            {x: MaxKey(), y: 10},
            {x: MaxKey(), y: MaxKey()},
            {x: 5, y: 20},
        ]),
    );
    moveChunkAndWait(ns, {x: 0});
    assertMigrationComplete(dbName, ns, coll, shard0DB, 3);
    coll.drop();
}

// Test 3: Compound shard key {x:1,y:1} with wider index {x:1,y:1,z:1} — both fixes combined.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("compound_wider", {x: 1, y: 1});
    assert.commandWorked(coll.createIndex({x: 1, y: 1, z: 1}));
    assert.commandWorked(coll.dropIndex({x: 1, y: 1}));
    assert.commandWorked(
        coll.insertMany([
            {x: MaxKey(), y: MaxKey(), z: 5},
            {x: MaxKey(), y: MaxKey(), z: MaxKey()},
            {x: 1, y: 2, z: 3},
        ]),
    );
    moveChunkAndWait(ns, {x: 0, y: 0});
    assertMigrationComplete(dbName, ns, coll, shard0DB, 3);
    coll.drop();
}

// Test 4: Non-last chunk migration — no regression.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("non_last", {x: 1});
    assert.commandWorked(
        coll.insertMany([
            {_id: "a", x: 1},
            {_id: "b", x: 5},
            {_id: "c", x: 10},
            {_id: "d", x: 15},
        ]),
    );
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}),
    );

    assert.eq(2, st.shard1.getDB(dbName).coll.find().itcount(), "shard1 has x=1 and x=5");
    assert.eq(4, coll.find().itcount(), "All 4 docs visible via mongos");
    assert.eq(2, shard0DB.coll.find().itcount(), "shard0 has x=10 and x=15");
    coll.drop();
}

// Test 5: Range deleter with failpoint — both exact and wider index.
{
    const shard0Admin = st.shard0.getDB("admin");

    // 5a: Exact index
    const t5a = setupCollection("rd_exact", {x: 1});
    assert.commandWorked(shard0Admin.runCommand({configureFailPoint: "suspendRangeDeletion", mode: "alwaysOn"}));
    assert.commandWorked(t5a.coll.insertMany([{x: MaxKey()}, {x: 5}]));
    assert.commandWorked(st.s.adminCommand({moveChunk: t5a.ns, find: {x: 0}, to: st.shard1.shardName}));
    assert.eq(2, st.shard1.getDB(t5a.dbName).coll.find().itcount(), "Both docs on shard1");
    assert.eq(2, t5a.shard0DB.coll.find().itcount(), "Orphans still on shard0");
    assert.commandWorked(shard0Admin.runCommand({configureFailPoint: "suspendRangeDeletion", mode: "off"}));
    assert.soon(() => t5a.shard0DB.coll.find().itcount() === 0, "Range deleter should clean up MaxKey orphans");
    t5a.coll.drop();

    // 5b: Wider index {x:1,y:1}
    const t5b = setupCollection("rd_wider", {x: 1});
    assert.commandWorked(t5b.coll.createIndex({x: 1, y: 1}));
    assert.commandWorked(t5b.coll.dropIndex({x: 1}));
    assert.commandWorked(shard0Admin.runCommand({configureFailPoint: "suspendRangeDeletion", mode: "alwaysOn"}));
    assert.commandWorked(
        t5b.coll.insertMany([
            {x: MaxKey(), y: 10},
            {x: 5, y: 20},
        ]),
    );
    assert.commandWorked(st.s.adminCommand({moveChunk: t5b.ns, find: {x: 0}, to: st.shard1.shardName}));
    assert.eq(2, st.shard1.getDB(t5b.dbName).coll.find().itcount(), "Both docs on shard1");
    assert.commandWorked(shard0Admin.runCommand({configureFailPoint: "suspendRangeDeletion", mode: "off"}));
    assert.soon(
        () => t5b.shard0DB.coll.find().itcount() === 0,
        "Range deleter with wider index should clean up MaxKey orphans",
    );
    t5b.coll.drop();
}

// Test 6: Partial MaxKey — compound shard key {x:1, y:1}.
// Docs where only some shard key fields are MaxKey (e.g. {x:MaxKey, y:10}) are below the
// global max boundary and should migrate regardless. Docs where ALL shard key fields are
// MaxKey exercise the exclusive upper bound fix.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("partial_max", {x: 1, y: 1});
    assert.commandWorked(
        coll.insertMany([
            {_id: "x_max_y_10", x: MaxKey(), y: 10},
            {_id: "x_10_y_max", x: 10, y: MaxKey()},
            {_id: "all_max", x: MaxKey(), y: MaxKey()},
            {_id: "normal", x: 1, y: 2},
        ]),
    );
    moveChunkAndWait(ns, {x: 0, y: 0});
    assertMigrationComplete(dbName, ns, coll, shard0DB, 4);
    coll.drop();
}

// Test 7: Zone bounds — extendRangeBound with prefix {x: MaxKey()} should pad trailing fields
// with MaxKey (not MinKey), producing {x: MaxKey(), y: MaxKey()}.
{
    const dbName = testName + "_zones";
    const ns = dbName + ".coll";

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1, y: 1}}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zone_all"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey()}, max: {x: MaxKey()}, zone: "zone_all"}),
    );

    const tags = st.s.getDB("config").tags.find({ns}).toArray();
    assert.eq(1, tags.length, "Should have exactly one zone tag");
    assert.eq(MaxKey(), tags[0].max.x, "Zone max x should be MaxKey");
    assert.eq(MaxKey(), tags[0].max.y, "Zone max y should be MaxKey (not MinKey)");

    assert.commandWorked(
        st.s.adminCommand({
            updateZoneKeyRange: ns,
            min: {x: MinKey(), y: MinKey()},
            max: {x: MaxKey(), y: MaxKey()},
            zone: null,
        }),
    );
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "zone_all"}));
    st.s.getDB(dbName).coll.drop();
}

// Test 8: Recipient pre-cloning check detects MaxKey orphans with wider-than-shard-key index.
// checkForExistingDocumentsInRange must extend bounds to the index width so that a MaxKey
// document already present on the recipient is detected even when the index is wider than
// the shard key.
{
    const {dbName, ns, coll, shard0DB} = setupCollection("recipient_wider_idx", {x: 1});
    assert.commandWorked(coll.createIndex({x: 1, y: 1}));
    assert.commandWorked(coll.dropIndex({x: 1}));

    assert.commandWorked(coll.insert({_id: "normal", x: 5}));

    // Directly insert a MaxKey orphan on shard1 (bypassing mongos to simulate an orphan from
    // a prior buggy migration that left it behind). The wider index {x:1, y:1} means the scan
    // must use extendRangeBound to pad bounds to match the index width.
    assert.commandWorked(st.shard1.getDB(dbName).coll.insert({_id: "orphan_maxkey", x: MaxKey(), y: 42}));

    // Migrate the chunk [MinKey, MaxKey) from shard0 to shard1. The recipient's pre-cloning
    // check should detect the orphaned MaxKey document and abort the migration. Without the
    // extendRangeBound fix, the wider index scan would use {x: MaxKey, y: MinKey} as the
    // upper bound, missing the orphan at {x: MaxKey, y: 42}.
    const result = st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true});

    assert.commandFailed(result);

    // Clean up the orphan directly on shard1.
    assert.commandWorked(st.shard1.getDB(dbName).coll.remove({_id: "orphan_maxkey"}));
    coll.drop();
}

st.stop();
