/**
 * Test that a reconfig that would change the implicit default write concern fails.
 * In order to perform such a reconfig, users must set a cluster-wide write concern.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

let cwwc = primary.adminCommand({getDefaultRWConcern: 1});
assert(cwwc.hasOwnProperty("defaultWriteConcern"));
assert.eq({w: "majority", wtimeout: 0}, cwwc.defaultWriteConcern, tojson(cwwc));

jsTestLog("Starting arbiter");
const arbiter = rst.add();
const config = rst.getReplSetConfigFromNode();
config.members.push({_id: 2, host: arbiter.host, arbiterOnly: true});
config.version++;
const reconfigErrorMsg = "Reconfig attempted to install a config that would change the implicit default write concern";

// Adding the arbiter would change the implicit default write concern from {w: majority} to
// {w:1}, so we fail the reconfig.
let res = assert.commandFailed(primary.adminCommand({replSetReconfig: config}));
assert.eq(res.code, ErrorCodes.NewReplicaSetConfigurationIncompatible);
assert(res.errmsg.includes(reconfigErrorMsg));

// A force reconfig should also fail.
res = assert.commandFailed(primary.adminCommand({replSetReconfig: config, force: true}));
assert.eq(res.code, ErrorCodes.NewReplicaSetConfigurationIncompatible);
assert(res.errmsg.includes(reconfigErrorMsg));

assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// After setting the cluster-wide write concern, the same reconfig command should succeed.
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

cwwc = primary.adminCommand({getDefaultRWConcern: 1});
assert(cwwc.hasOwnProperty("defaultWriteConcern"));
assert.eq({w: 1, wtimeout: 0}, cwwc.defaultWriteConcern, tojson(cwwc));

rst.stopSet();
