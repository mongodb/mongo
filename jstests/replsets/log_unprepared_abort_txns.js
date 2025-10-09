/**
 * Check that the primary logs all unprepared transactions that were aborted
 * during stepdown.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({
    nodes: [{}],
});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB("test");

jsTest.log.info("Start some transactions without committing.");
const sessions = [];
for (let i = 0; i < 10; i++) {
    const sessionId = UUID("00000000-0000-0000-0000-00000000000" + i);
    assert.commandWorked(
        db.runCommand({
            insert: "coll",
            documents: [{_id: i}],
            txnNumber: NumberLong(i),
            lsid: {id: sessionId},
            startTransaction: true,
            autocommit: false,
        }),
    );
}

jsTest.log.info("Step the primary down.");
assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));

jsTest.log.info("Check that the primary logs aborted transactions");
for (let i = 0; i < 10; i++) {
    checkLog.contains(
        primary,
        new RegExp(
            `11101700.*00000000-0000-0000-0000-00000000000${i}.*"txnNumber":${i}.*"reason":${ErrorCodes.InterruptedDueToReplStateChange}`,
        ),
    );
}

replTest.stopSet();
