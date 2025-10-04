/**
 * Test that protocolVersion defaults to 1 even during a replSetReconfig.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
let config = primary.getDB("local").system.replset.findOne();
config.version++;
delete config.protocolVersion;

assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

// Make sure that the config still has the proper protocolVersion.
config = primary.getDB("local").system.replset.findOne();
assert.eq(config.protocolVersion, 1);

rst.stopSet();
