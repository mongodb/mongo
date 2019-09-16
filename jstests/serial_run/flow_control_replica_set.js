/**
 * This test artificially throttles a replica set by limiting the tickets handed out. It first
 * performs a calibrating run that sees how many inserts per second a one node replica set can
 * handle. Non-batch inserts should acquire one lock per insert. The test then sets the ticket
 * generation to a fraction of this discovered calibration value. A following benchrun validates the
 * new insert rate falls within some (generous) range.
 *
 * @tags: [
 *   requires_replication,
 *   requires_flow_control,
 *   requires_majority_read_concern,
 * ]
 */
(function() {
"use strict";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();

assert.commandWorked(primary.adminCommand({
    configureFailPoint: "flowControlTicketOverride",
    mode: "alwaysOn",
    data: {"numTickets": 1000 * 1000 * 1000}
}));
// Sleep 2 seconds for the failpoint to take effect.
sleep(2000);

let result = benchRun({
    host: primary.host,
    seconds: 5,
    parallel: 5,
    ops: [{op: "insert", ns: "foo.bar", doc: {field: "value"}}]
});
jsTestLog({CalibratingRun: result});

let insertRate = result["insert"];
let throttledRate = insertRate / 2;
assert.commandWorked(primary.adminCommand({
    configureFailPoint: "flowControlTicketOverride",
    mode: "alwaysOn",
    data: {"numTickets": NumberInt(throttledRate)}
}));
// Sleep 2 seconds for the failpoint to take effect.
sleep(2000);

result = benchRun({
    host: primary.host,
    seconds: 5,
    parallel: 5,
    ops: [{op: "insert", ns: "foo.bar", doc: {field: "value"}}]
});
jsTestLog({ThrottledRun: result, ThrottedRate: throttledRate});
let maxAllowedRate = 1.5 * throttledRate;
let minAllowedRate = 0.5 * throttledRate;
assert.gt(result["insert"], minAllowedRate);
assert.lt(result["insert"], maxAllowedRate);

// Cautiously unset to avoid any interaction with shutdown.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "flowControlTicketOverride", mode: "off"}));

replTest.stopSet();
})();
