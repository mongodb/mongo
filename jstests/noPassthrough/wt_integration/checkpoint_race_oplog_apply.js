/**
 * Empirical repro for the bug formalised in
 * src/mongo/tla_plus/Storage/CheckpointOplogTableVisibility:
 *
 *   Installing a WiredTiger checkpoint can race with secondary oplog application.
 *   If a checkpoint install fires between
 *     (a) creating the storage table for a new collection, and
 *     (b) advancing lastApplied past the create-collection oplog op,
 *   the checkpoint persists the old catalog snapshot and the just-created table
 *   "ceases to exist" on disk. The TLA+ spec captures the race; this test
 *   exercises the same window with real ReplSetTest traffic.
 *
 * Strategy:
 *   - Two-node replica set. Secondary tail-applies oplog from the primary.
 *   - On the secondary, drop syncdelay to its minimum so the WiredTiger
 *     checkpoint thread fires aggressively in the background.
 *   - On the primary, hammer createCollection / drop / re-create / insert for
 *     many collections so the secondary has a steady stream of create-collection
 *     oplog entries to interleave with checkpoint installs.
 *   - After each batch, force an explicit fsync checkpoint on the secondary to
 *     also exercise the synchronous install path.
 *   - Assert the secondary log contains no "TableNotFound" / "ident not found"
 *     errors. The TLA+ NoUseOfMissingTable invariant is the formal twin of this
 *     log scrape: any TableNotFound is exactly a violation of that invariant in
 *     production.
 *
 * The window is intentionally tight (the ticket notes "the timing required to
 * hit this is sufficiently specific that it might be impossible for it to
 * happen in practice"), so this test focuses on building enough oplog-apply +
 * checkpoint pressure to surface the race if it ever can be hit, and on
 * pinning the absence of TableNotFound on the secondary as a regression guard.
 *
 * @tags: [
 *   requires_fsync,
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const kNumCollections = 25;
const kRoundsOfCreates = 8;
const kInsertsPerRound = 20;

// Minimum syncdelay (in seconds) that WiredTiger accepts. 1s is the documented
// floor; we want the checkpoint thread to fire as often as possible.
const kAggressiveSyncDelaySecs = 1;

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            rsConfig: {priority: 0, votes: 0},
        },
    ],
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// On the secondary, set syncdelay to its aggressive floor so the checkpoint
// thread is constantly trying to install while we feed it create-collection
// oplog entries.
assert.commandWorked(
    secondary.adminCommand({setParameter: 1, syncdelay: kAggressiveSyncDelaySecs}),
);

// Crank storage logging on the secondary so any TableNotFound or "ident not
// found" message lands in the log where checkLog can see it.
assert.commandWorked(secondary.setLogLevel(1, "storage"));

const primaryDB = primary.getDB(dbName);

jsTestLog("Round-robin createCollection / drop / insert to hammer oplog apply + checkpoint race");

for (let round = 0; round < kRoundsOfCreates; round++) {
    for (let i = 0; i < kNumCollections; i++) {
        const collName = `coll_${round}_${i}`;
        // Create + insert + drop in tight sequence so each round produces a
        // burst of catalog mutations the secondary has to apply.
        assert.commandWorked(primaryDB.createCollection(collName));
        const coll = primaryDB.getCollection(collName);
        const docs = [];
        for (let j = 0; j < kInsertsPerRound; j++) {
            docs.push({_id: j, round: round, idx: i, x: j});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({x: 1}));
        // Drop half of them within the round; the other half stay around so the
        // secondary has live tables to use and check.
        if (i % 2 === 0) {
            assert(coll.drop());
        }
    }

    // Force a synchronous checkpoint on the secondary in addition to the
    // background syncdelay-driven ones. This is the other side of the race
    // window: a foreground fsync can fire while a create-collection oplog op
    // is mid-phase on the applier.
    assert.commandWorked(secondary.getDB("admin").runCommand({fsync: 1}));

    // Let the secondary catch up before the next round so we don't blow the
    // oplog window, but don't drain to quiescence -- we want some overlap.
    rst.awaitReplication();
}

jsTestLog("Final replication catch-up");
rst.awaitReplication();

// One more synchronous checkpoint after replication has caught up; this is the
// post-race quiescent state. Under the bug the secondary may already have
// crashed with TableNotFound, but if it survived, this catches any remaining
// "table referenced by catalog but missing on disk" state.
assert.commandWorked(secondary.getDB("admin").runCommand({fsync: 1}));

jsTestLog("Asserting no TableNotFound / missing-ident errors on the secondary");

// Grab the secondary log and scan for the bug surface. We accept several
// renderings WiredTiger / storage uses for the same underlying condition.
const log = assert.commandWorked(secondary.adminCommand({getLog: "global"})).log;
const offendingPatterns = [
    /TableNotFound/,
    /ident not found/i,
    /no such table/i,
    /WT_NOTFOUND.*table/i,
];

let offending = [];
for (const line of log) {
    for (const pat of offendingPatterns) {
        if (pat.test(line)) {
            offending.push(line);
            break;
        }
    }
}

assert.eq(
    offending.length,
    0,
    "Secondary log contains TableNotFound / missing-ident entries, indicating a checkpoint/oplog race: " +
        tojson(offending.slice(0, 5)),
);

// Sanity: secondary must still be SECONDARY (i.e. it didn't crash and get
// restarted into a different state during the run).
const secondaryStatus = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert.eq(
    secondaryStatus.myState,
    2 /* SECONDARY */,
    "Secondary is not in SECONDARY state after the race-window workload: " + tojson(secondaryStatus),
);

rst.stopSet();
