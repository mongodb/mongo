/**
 * This tests that secondaryOk'd queries in sharded setups get correctly routed when a secondary
 * goes into RECOVERING state, and don't break
 */

// Shard secondaries are restarted, which may cause that shard's primary to stepdown while it does
// not see the secondaries. Either the primary connection gets reset, or the primary could change.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let shardTest = new ShardingTest({name: "recovering_secondaryok", shards: 2, mongos: 2, other: {rs: true}});

let mongos = shardTest.s0;
let mongosSOK = shardTest.s1;
mongosSOK.setSecondaryOk();

const dbName = "test";
let dbase = mongos.getDB(dbName);
let coll = dbase.getCollection("foo");
let collSOk = mongosSOK.getCollection("" + coll);

let rsA = shardTest.rs0;
let rsB = shardTest.rs1;

assert.commandWorked(rsA.getPrimary().getDB("test_a").dummy.insert({x: 1}));
assert.commandWorked(rsB.getPrimary().getDB("test_b").dummy.insert({x: 1}));

rsA.awaitReplication();
rsB.awaitReplication();

print("1: initial insert");

assert.commandWorked(coll.save({_id: -1, a: "a", date: new Date()}));
assert.commandWorked(coll.save({_id: 1, b: "b", date: new Date()}));

print("2: shard collection");

shardTest.shardColl(
    coll,
    /* shardBy */ {_id: 1},
    /* splitAt */ {_id: 0},
    /* move chunk */ {_id: 0},
    /* dbname */ null,
    /* waitForDelete */ true,
);

print("3: test normal and secondaryOk queries");

// Make shardA and rsA the same
let shardA = shardTest.getShard(coll, {_id: -1});
let shardAColl = shardA.getCollection("" + coll);

if (shardA.name == rsB.getURL()) {
    let swap = rsB;
    rsB = rsA;
    rsA = swap;
}

rsA.awaitReplication();
rsB.awaitReplication();

// Because of async migration cleanup, we need to wait for this condition to be true
assert.soon(function () {
    return coll.find().itcount() == collSOk.find().itcount();
});

assert.eq(shardAColl.find().itcount(), 1);
assert.eq(shardAColl.findOne()._id, -1);

print("5: make one of the secondaries RECOVERING");

let secs = rsA.getSecondaries();
let goodSec = secs[0];
let badSec = secs[1];

assert.commandWorked(badSec.adminCommand("replSetMaintenance"));
rsA.waitForState(badSec, ReplSetTest.State.RECOVERING);

print("6: stop non-RECOVERING secondary");

rsA.stop(goodSec);

print("7: check our regular and secondaryOk query");

assert.eq(2, coll.find().itcount());
assert.eq(2, collSOk.find().itcount());

print("8: restart both our secondaries clean");

rsA.getSecondaries().forEach((secondary) =>
    rsA.restart(secondary, {remember: true, startClean: true}, undefined, 5 * 60 * 1000),
);

print("9: wait for recovery");

rsA.awaitSecondaryNodes(5 * 60 * 1000);

print("10: check our regular and secondaryOk query");

// We need to make sure our nodes are considered accessible from mongos - otherwise we fail
// See SERVER-7274
awaitRSClientHosts(coll.getMongo(), rsA.nodes, {ok: true});
awaitRSClientHosts(coll.getMongo(), rsB.nodes, {ok: true});

// We need to make sure at least one secondary is accessible from mongos - otherwise we fail
// See SERVER-7699
awaitRSClientHosts(collSOk.getMongo(), [rsA.getSecondaries()[0]], {secondary: true, ok: true});
awaitRSClientHosts(collSOk.getMongo(), [rsB.getSecondaries()[0]], {secondary: true, ok: true});

print("SecondaryOk Query...");
let sOKCount = collSOk.find().itcount();

let collCount = null;
try {
    print("Normal query...");
    collCount = coll.find().itcount();
} catch (e) {
    printjson(e);

    // There may have been a stepdown caused by step 8, so we run this twice in a row. The first
    // time can error out.
    print("Error may have been caused by stepdown, try again.");
    collCount = coll.find().itcount();
}

assert.eq(collCount, sOKCount);

shardTest.stop();
