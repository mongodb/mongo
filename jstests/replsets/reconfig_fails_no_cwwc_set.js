/*
 * Test that a reconfig that would change the implicit default write concern fails.
 * In order to perform such a reconfig, users must set a cluster-wide write concern.
 *
 * @tags: [requires_fcv_50]
 */

(function() {
'use strict';

load("jstests/libs/write_concern_util.js");  // For 'isDefaultWriteConcernMajorityFlagEnabled()'.

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

// If the cluster-wide write concern has not been set, 'defaultWriteConcern' will be omitted from
// the 'getDefaultRWConcern' response object.
let cwwc = primary.adminCommand({getDefaultRWConcern: 1});
if (isDefaultWriteConcernMajorityFlagEnabled(primary)) {
    assert(cwwc.hasOwnProperty("defaultWriteConcern"));
    assert.eq({w: "majority", wtimeout: 0}, cwwc.defaultWriteConcern, tojson(cwwc));
} else {
    assert(!cwwc.hasOwnProperty("defaultWriteConcern"));
}

jsTestLog("Starting arbiter");
const arbiter = rst.add();
const config = rst.getReplSetConfigFromNode();
config.members.push({_id: 2, host: arbiter.host, arbiterOnly: true});
config.version++;

// Adding the arbiter would change the implicit default write concern from {w: majority} to {w: 1},
// so we fail the reconfig.
let res = assert.commandFailed(primary.adminCommand({replSetReconfig: config}));
assert.eq(res.code, ErrorCodes.NewReplicaSetConfigurationIncompatible);
assert(res.errmsg.includes(
    "Reconfig attempted to install a config that would change the implicit default write concern"));

// A force reconfig should also fail.
res = assert.commandFailed(primary.adminCommand({replSetReconfig: config, force: true}));
assert.eq(res.code, ErrorCodes.NewReplicaSetConfigurationIncompatible);
assert(res.errmsg.includes(
    "Reconfig attempted to install a config that would change the implicit default write concern"));

assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// After setting the cluster-wide write concern, the same reconfig command should succeed.
res = assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
assert(res.ok);

cwwc = primary.adminCommand({getDefaultRWConcern: 1});
assert(cwwc.hasOwnProperty("defaultWriteConcern"));
assert.eq({w: 1, wtimeout: 0}, cwwc.defaultWriteConcern, tojson(cwwc));

rst.stopSet();
})();
