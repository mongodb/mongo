/**
 * Tests that resharding can handle the case where there is replication lag on donor shards. That
 * is, when the recipient is approaching strict consistency, the resharding oplog fetcher starts
 * targeting the primary node of the donor shard instead of the nearest node to prepare for the
 * critical section.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

/**
 * Returns profiler entries for the resharding oplog fetcher aggregate command.
 */
function getProfilerEntries(conn) {
    return conn.getDB("local")
        .system.profile
        .find({
            op: "command",
            ns: "local.oplog.rs",
            "command.aggregate": "oplog.rs",
        })
        .toArray()
        .filter(entry => {
            for (let stage of entry.command.pipeline) {
                if (stage.hasOwnProperty("$_internalReshardingIterateTransaction")) {
                    return true;
                }
            }
            return false;
        });
}

/**
 * Returns the read preference used by the command with the given profiler entry.
 */
function getReadPreference(profilerEntry) {
    assert(profilerEntry.hasOwnProperty("command"), profilerEntry);
    assert(profilerEntry.command.hasOwnProperty("$queryOptions"), profilerEntry);
    assert(profilerEntry.command["$queryOptions"].hasOwnProperty("$readPreference"), profilerEntry);
    return profilerEntry.command["$queryOptions"]["$readPreference"].mode;
}

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 3,
        // Disallow chaining to force both secondaries to sync from the primary. This disables
        // replication on one of the secondaries, with chaining that would effectively disable
        // replication on both secondaries, causing the test setup to be wrong since writeConcern of
        // w: majority is unsatisfiable.
        settings: {chainingAllowed: false},
    }
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const testColl = st.s.getCollection(ns);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));

const configPrimary = st.configRS.getPrimary();
const donorPrimary = st.rs0.getPrimary();

function runMoveCollection(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveCollection: ns, toShard});
}

// Turn on profiling on all nodes on the donor shard.
st.rs0.nodes.forEach(node => node.getDB("local").setProfilingLevel(2));

const beforeQueryingRemainingTimeFp =
    configureFailPoint(configPrimary, "hangBeforeQueryingRecipients");
const beforeCriticalSectionFp =
    configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");
const moveThread = new Thread(runMoveCollection, st.s.host, ns, st.shard1.shardName);
moveThread.start();

beforeQueryingRemainingTimeFp.wait();

// Verify that the resharding oplog fetcher initially fetches from the nearest node.
let numEntriesNearestReadPref = 0;
st.rs0.nodes.forEach(node => {
    const profilerEntries = getProfilerEntries(node);
    profilerEntries.forEach(profilerEntry => {
        const readPref = getReadPreference(profilerEntry);
        assert.eq(readPref, "nearest");
        numEntriesNearestReadPref++;
    });
});
assert.gt(
    numEntriesNearestReadPref,
    0,
    "Expected to find resharding oplog fetcher profiler entries with read preference 'nearest'");

beforeQueryingRemainingTimeFp.off();
beforeCriticalSectionFp.wait();

// Verify that the resharding oplog fetcher eventually starts fetching from the primary now that the
// recipient is approaching strict consistency to prepare for the critical section.
let numEntriesPrimaryReadPrefBefore = 0;
assert.soon(() => {
    numEntriesPrimaryReadPrefBefore = 0;
    st.rs0.nodes.forEach(node => {
        const profilerEntries = getProfilerEntries(node);
        if (node == donorPrimary) {
            profilerEntries.forEach(profilerEntry => {
                const readPref = getReadPreference(profilerEntry);
                assert(readPref == "primary" || readPref == "nearest", readPref);
                if (readPref == "primary") {
                    numEntriesPrimaryReadPrefBefore++;
                }
            });
        } else {
            profilerEntries.forEach(profilerEntry => {
                const readPref = getReadPreference(profilerEntry);
                assert.eq(readPref, "nearest");
            });
            // Drop the profiler entries on the secondaries so that we can verify later that the
            // resharding oplog fetcher was still fetching from the primary after the critical
            // section had started.
            node.getDB("local").setProfilingLevel(0);
            assert(node.getDB("local").system.profile.drop());
            node.getDB("local").setProfilingLevel(2);
        }
    });
    return numEntriesPrimaryReadPrefBefore > 0;
}, "Expected to find resharding oplog fetcher profiler entries with read preference 'primary'");

// Pause replication on one of the secondaries so that if the resharding oplog fetcher targets
// nearest node instead of the primary at any point after this, the resharding operation could get
// stuck in the critical section.
stopServerReplication(st.rs0.getSecondaries()[1]);

beforeCriticalSectionFp.off();
assert.commandWorked(moveThread.returnData());

// Verify that the resharding oplog fetcher was still fetching from the primary after the critical
// section had started.
let numEntriesPrimaryReadPrefAfter = 0;
st.rs0.nodes.forEach(node => {
    const profilerEntries = getProfilerEntries(node);
    if (node == donorPrimary) {
        profilerEntries.forEach(profilerEntry => {
            const readPref = getReadPreference(profilerEntry);
            assert(readPref == "primary" || readPref == "nearest", readPref);
            if (readPref == "primary") {
                numEntriesPrimaryReadPrefAfter++;
            }
        });
    } else {
        assert.eq(profilerEntries.length, 0, {profilerEntries});
    }
});
assert.gte(
    numEntriesPrimaryReadPrefAfter,
    numEntriesPrimaryReadPrefBefore,
    "Expected to find at least as many resharding oplog fetcher profiler entries with read " +
        "preference 'primary' before and after the critical section");

jsTest.log("Profiler entry counts: " + tojson({
               numEntriesNearestReadPref,
               numEntriesPrimaryReadPrefBefore,
               numEntriesPrimaryReadPrefAfter
           }));

restartServerReplication(st.rs0.getSecondaries()[1]);
st.stop();
