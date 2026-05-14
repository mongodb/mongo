/**
 * Verify that toggling 'ttlMonitorEnabled' at runtime correctly pauses and resumes TTL
 * deletions. With the monitor disabled, expired documents must persist across passes; once
 * re-enabled, the next pass must reclaim them.
 *
 * SERVER-96179: extends TTL unit testing for runtime parameter toggles.
 */
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const conn = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
const db = conn.getDB("test");
const coll = db.ttl_runtime_toggle;
coll.drop();

// Disable the monitor before inserting expired docs so the first observation is deterministic.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0}));

const now = new Date();
const past = new Date(now.getTime() - 3600 * 1000 * 24);
for (let i = 0; i < 50; i++) {
    assert.commandWorked(coll.insert({x: past}));
}
assert.eq(50, coll.find().itcount(), "expired docs should not be reaped while monitor is disabled");

// Sleep enough wall-clock for several would-be passes; documents must remain.
const passesAtDisable = db.serverStatus().metrics.ttl.passes;
sleep(3000);
assert.eq(
    passesAtDisable,
    db.serverStatus().metrics.ttl.passes,
    "ttl.passes must not advance while ttlMonitorEnabled=false",
);
assert.eq(50, coll.find().itcount(), "documents must persist while monitor is disabled");

// Re-enable; expired docs should be reaped on the next pass.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
TTLUtil.waitForPass(db);
assert.eq(0, coll.find().itcount(), "all expired docs should be deleted after re-enabling monitor");

// Toggle off again, insert more expired docs, confirm they survive.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));
for (let i = 0; i < 25; i++) {
    assert.commandWorked(coll.insert({x: past}));
}
const passesAfterReDisable = db.serverStatus().metrics.ttl.passes;
sleep(2500);
assert.eq(
    passesAfterReDisable,
    db.serverStatus().metrics.ttl.passes,
    "ttl.passes must not advance after re-disabling monitor",
);
assert.eq(25, coll.find().itcount(), "re-inserted expired docs must persist with monitor off");

// Final re-enable to confirm symmetry.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));
TTLUtil.waitForPass(db);
assert.eq(0, coll.find().itcount(), "all docs should drain after final re-enable");

MongoRunner.stopMongod(conn);
