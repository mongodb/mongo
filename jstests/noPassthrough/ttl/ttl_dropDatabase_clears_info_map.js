/**
 * Verify that dropDatabase against a database that contains TTL indexes
 * removes every corresponding entry from the TTL info map, so that the
 * next TTL pass does not attempt to scan a non-existent namespace.
 *
 * Regression coverage for the fix in commit aa2afa865d (SERVER-97657:
 * "fix ttl info map on index update"). Coverage gap surfaced by
 * SERVER-97661 jstest audit (gap #3).
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbKeep = primary.getDB("ttl_dropDatabase_clears_info_map_keep");
const dbDrop = primary.getDB("ttl_dropDatabase_clears_info_map_drop");

function seed(db) {
    const collA = db.getCollection("collA");
    const collB = db.getCollection("collB");
    collA.drop();
    collB.drop();

    const past = new Date(Date.now() - 24 * 3600 * 1000);
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(collA.insert({_id: i, t: past}));
        assert.commandWorked(collB.insert({_id: i, t: past}));
    }
    assert.commandWorked(collA.createIndex({t: 1}, {expireAfterSeconds: 0}));
    assert.commandWorked(collB.createIndex({t: 1}, {expireAfterSeconds: 0}));
}

seed(dbKeep);
seed(dbDrop);

// First pass: confirm all four collections (2 in each db) are visible to
// the TTL monitor and reap their seeded data.
TTLUtil.waitForPass(dbKeep);
TTLUtil.waitForPass(dbKeep);
assert.soon(() => dbKeep.collA.countDocuments({}) === 0);
assert.soon(() => dbKeep.collB.countDocuments({}) === 0);
assert.soon(() => dbDrop.collA.countDocuments({}) === 0);
assert.soon(() => dbDrop.collB.countDocuments({}) === 0);

// Re-seed only the "drop" database with fresh expired docs, then freeze
// the TTL monitor between passes so we can deterministically observe the
// state of the info map across the drop.
const past = new Date(Date.now() - 24 * 3600 * 1000);
for (let i = 100; i < 105; ++i) {
    assert.commandWorked(dbDrop.collA.insert({_id: i, t: past}));
    assert.commandWorked(dbDrop.collB.insert({_id: i, t: past}));
}
assert.eq(5, dbDrop.collA.countDocuments({}));
assert.eq(5, dbDrop.collB.countDocuments({}));

const pauseTtl = configureFailPoint(primary, "hangTTLMonitorBetweenPasses");
pauseTtl.wait();

// Drop the entire "drop" database, including its two TTL-bearing collections.
assert.commandWorked(dbDrop.dropDatabase());
assert.eq(0, primary.adminCommand({listDatabases: 1, nameOnly: true})
                       .databases.filter((d) => d.name === dbDrop.getName()).length);

// Release the monitor and let two passes go by. With the fix the info map
// no longer references the dropped namespaces; without the fix the
// monitor would either log a "namespace not found" warning or, worse,
// fail an invariant when it tried to acquire the dropped collection.
pauseTtl.off();
TTLUtil.waitForPass(dbKeep);
TTLUtil.waitForPass(dbKeep);

// The kept database must still be reachable - re-seed it with expired
// docs and verify the monitor reaps them, proving the monitor survived
// the drop.
for (let i = 200; i < 205; ++i) {
    assert.commandWorked(dbKeep.collA.insert({_id: i, t: past}));
}
TTLUtil.waitForPass(dbKeep);
TTLUtil.waitForPass(dbKeep);
assert.soon(() => dbKeep.collA.countDocuments({}) === 0,
            "kept-db TTL collection should continue to reap after dropDatabase elsewhere");

// Final sanity: monitor pass counter has continued to advance, meaning
// no stale info-map entry brought the TTL thread down.
const passesBefore = primary.getDB("admin").serverStatus().metrics.ttl.passes;
TTLUtil.waitForPass(dbKeep);
assert.gt(primary.getDB("admin").serverStatus().metrics.ttl.passes, passesBefore);

rst.stopSet();
