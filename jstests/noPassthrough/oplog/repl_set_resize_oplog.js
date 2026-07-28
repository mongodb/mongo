/**
 * Tests that resizing the oplog works as expected and validates input arguments.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

let replSet = new ReplSetTest({nodes: 2, oplogSize: 50});
replSet.startSet({
    oplogMinRetentionHours: 24,
});
replSet.initiate();

let primary = replSet.getPrimary();

const MB = 1024 * 1024;
const GB = 1024 * MB;
const PB = 1024 * GB;
const EB = 1024 * PB;

assert.eq(primary.getDB("local").oplog.rs.stats().maxSize, 50 * MB);
let serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert.eq(serverStatus.oplogTruncation.oplogMinRetentionHours, 24);

// Too small: 990MB
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: 900}),
    [ErrorCodes.BadValue, 51024], // getting BadValue when binary is > 7.1, else 51024
    "Expected replSetResizeOplog to fail because the size was too small",
);

// Way too small: -1GB
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: (-1 * GB) / MB}),
    [ErrorCodes.BadValue, 51024], // getting BadValue when binary is > 7.1, else 51024
    "Expected replSetResizeOplog to fail because the size was too small",
);

// Too big: 8EB
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: (8 * EB) / MB}),
    [ErrorCodes.BadValue, 51024], // getting BadValue when binary is > 7.1, else 51024
    "Expected replSetResizeOplog to fail because the size was too big",
);

// Min Retention Hours not valid: -1hr
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: 990, minRetentionHours: -1}),
    [ErrorCodes.BadValue, 51024], // getting BadValue when binary is > 7.1, else 51024
    "Expected replSetResizeOplog to fail because the minimum retention hours was too low",
);

// The maximum: 1PB
assert.commandWorked(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: (1 * PB) / MB}),
);

// Valid size and minRetentionHours
assert.commandWorked(
    primary
        .getDB("admin")
        .runCommand({replSetResizeOplog: 1, size: (1 * PB) / MB, minRetentionHours: 5}),
);
serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert.eq(serverStatus.oplogTruncation.oplogMinRetentionHours, 5);

// Valid minRetentionHours with no size parameter.
assert.commandWorked(
    primary.getDB("admin").runCommand({replSetResizeOplog: 1, minRetentionHours: 1}),
);

assert.eq(primary.getDB("local").oplog.rs.stats().maxSize, 1 * PB);
serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert.eq(serverStatus.oplogTruncation.oplogMinRetentionHours, 1);

// TODO (SERVER-131719): Remove this workaround
// In disaggregated storage, listCollections reads from the secondary's in-memory catalog while
// listCatalog reads from the shared durable catalog, which is updated by the primary and may
// reflect the result of replSetResizeOplog from the primary. This can cause a shutdown consistency
// check to fail because listCollections and listCatalog on the secondary have different
// options.size values for the oplog.rs collection.
// As a workaround, set the secondary to the same value as the primary.
let secondary = replSet.getSecondary();
assert.commandWorked(
    secondary
        .getDB("admin")
        .runCommand({replSetResizeOplog: 1, size: (1 * PB) / MB, minRetentionHours: 1}),
);
assert.eq(
    primary.getDB("local").oplog.rs.stats().maxSize,
    secondary.getDB("local").oplog.rs.stats().maxSize,
);

replSet.stopSet();
