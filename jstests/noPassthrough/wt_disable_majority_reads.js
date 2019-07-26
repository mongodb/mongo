// @tags: [requires_wiredtiger, requires_replication]
(function() {
"use strict";

var rst = new ReplSetTest({
    nodes: [
        {"enableMajorityReadConcern": ""},
        {"enableMajorityReadConcern": "false"},
        {"enableMajorityReadConcern": "true"}
    ]
});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

rst.getPrimary().getDB("test").getCollection("test").insert({});
rst.awaitReplication();

// Node 0 is using the default, which is `enableMajorityReadConcern: true`. Thus a majority
// read should succeed.
assert.commandWorked(
    rst.nodes[0].getDB("test").runCommand({"find": "test", "readConcern": {"level": "majority"}}));
// Node 1 disables majority reads. Check for the appropriate error code.
assert.commandFailedWithCode(
    rst.nodes[1].getDB("test").runCommand({"find": "test", "readConcern": {"level": "majority"}}),
    ErrorCodes.ReadConcernMajorityNotEnabled);
// Same as Node 0.
assert.commandWorked(
    rst.nodes[2].getDB("test").runCommand({"find": "test", "readConcern": {"level": "majority"}}));

rst.stopSet();
})();
