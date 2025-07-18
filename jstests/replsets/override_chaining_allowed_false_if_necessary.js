/**
 * N0(p), N1(s), N2(s)
 * N1 and N2 have priority 0, cannot be primary
 * N0 gets restarted, and after oplog truncation, is behind N1 and N2
 *
 * If we don't override `chainingAllowed: false` for N0, then it cannot
 * sync its missing oplog entries, and none of the nodes in the replica set
 * will be allowed to step up.
 *
 * @tags: [
 *  requires_journaling,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "no_stepup_when_primary_behind";
let rst = new ReplSetTest({
    name: name,
    nodes: {
        n0: {},
        n1: {},
        n2: {},
    },
});
rst.startSet();
const timeoutMS = 10 * 1000;

const conf = rst.getReplSetConfig();
conf.members[1].priority = 0;
conf.members[2].priority = 0;
conf.settings = {
    heartbeatIntervalMillis: 500,
    electionTimeoutMillis: 5000,
    chainingAllowed: false,
};

rst.initiate(conf);
rst.awaitNodesAgreeOnPrimary();

const primary = rst.getPrimary();

assert.commandWorked(
    primary.getDB("test").test.insert({key0: "value0"}, {writeConcern: {w: 3}}),
);

// Pause the journal flusher so that N0 will be behind N1 and N2 after a restart.
const pauseJournalFlusherThreadName = "pauseJournalFlusherThread";
const pauseJournalFlusherThreadFp = configureFailPoint(
    primary,
    pauseJournalFlusherThreadName,
);
pauseJournalFlusherThreadFp.wait();

assert.commandWorked(primary.getDB("test").test.insert({key1: "value1"}));

rst.stop(primary);

rst.awaitNoPrimary(
    "Ensure that the primary was successfully terminated.",
    timeoutMS,
);

rst.start(primary, {waitForConnect: true}, true, true);

jsTestLog("Kill and restart complete");

assert(rst.getPrimary(timeoutMS));

rst.stopSet();
