/**
 * Tests that the server prints storage statistics along with slow secondary oplog application
 * logs.
 *
 * @tags: [requires_wiredtiger, requires_persistence, requires_fcv_73]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {setLogVerbosity} from "jstests/replsets/rslib.js";

const name = "log_wt_stats_during_secondary_oplog_application";
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

// Create a collection and write some data to it.
assert.commandWorked(primary.getDB(name).createCollection("readFromDisk"));
assert.commandWorked(primary.getDB(name)["readFromDisk"].insert({x: "value"}));

// Cleanly shut down the secondary and restart it. This will clear the local cache and ensure
// that we need to read from disk the next time we write to the collection, resulting in
// actual storage statistics instead of empty storage statistics.
rst.stop(secondary, undefined /* signal */, {} /* opts */, {forRestart: true});
rst.start(secondary, {} /* options */, true /* restart */, false /* waitForHealth */);

// Ensure secondary completes startup recovery.
secondary = rst.getSecondary();

// Set profiling level to 2 so that we log all operations.
assert.commandWorked(secondary.getDB(name).setProfilingLevel(2, 0));

// Issue a new write to the primary that will require reading from disk.
assert.commandWorked(primary.getDB(name)["readFromDisk"].insert({x: "sloth"}));
rst.awaitReplication();

// Make sure we log that insert op.
const slowLogLine = checkLog.containsLog(secondary, "sloth");
jsTestLog(slowLogLine);

// Make sure we've logged a storage statistic as well.
assert(slowLogLine.includes("bytesRead"));

rst.stopSet();
