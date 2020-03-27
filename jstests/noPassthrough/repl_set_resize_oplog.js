/**
 * Tests that resizing the oplog works as expected and validates input arguments.
 *
 * @tags: [requires_replication, requires_wiredtiger]
 */
(function() {
"use strict";

let replSet = new ReplSetTest({nodes: 2, oplogSize: 50});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();

const MB = 1024 * 1024;
const GB = 1024 * MB;
const PB = 1024 * GB;
const EB = 1024 * PB;

assert.eq(primary.getDB('local').oplog.rs.stats().maxSize, 50 * MB);

// Too small: 990MB
assert.commandFailedWithCode(primary.getDB('admin').runCommand({replSetResizeOplog: 1, size: 900}),
                             51024,  // IDL Error code for invalid fields
                             "Expected replSetResizeOplog to fail because the size was too small");

// Way too small: -1GB
assert.commandFailedWithCode(
    primary.getDB('admin').runCommand({replSetResizeOplog: 1, size: -1 * GB / MB}),
    51024,  // IDL Error code for invalid fields
    "Expected replSetResizeOplog to fail because the size was too small");

// Too big: 8EB
assert.commandFailedWithCode(
    primary.getDB('admin').runCommand({replSetResizeOplog: 1, size: 8 * EB / MB}),
    51024,  // IDL Error code for invalid fields
    "Expected replSetResizeOplog to fail because the size was too big");

// Min Retention Hours not valid: -1hr
assert.commandFailedWithCode(
    primary.getDB('admin').runCommand({replSetResizeOplog: 1, size: 990, minRetentionHours: -1}),
    51024,  // IDL Error code for invalid fields
    "Expected replSetResizeOplog to fail because the minimum retention hours was too low");

// The maximum: 1PB
assert.commandWorked(primary.getDB('admin').runCommand({replSetResizeOplog: 1, size: 1 * PB / MB}));

// Valid size and minRetentionHours
assert.commandWorked(primary.getDB('admin').runCommand(
    {replSetResizeOplog: 1, size: 1 * PB / MB, minRetentionHours: 5}));

// Valid minRetentionHours with no size parameter.
assert.commandWorked(
    primary.getDB('admin').runCommand({replSetResizeOplog: 1, minRetentionHours: 1}));

assert.eq(primary.getDB('local').oplog.rs.stats().maxSize, 1 * PB);

replSet.stopSet();
})();
