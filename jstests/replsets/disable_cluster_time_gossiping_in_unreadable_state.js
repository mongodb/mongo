/**
 * Verifies cluster time metadata is not gossiped or processed by nodes in an unreadable state.
 */
(function() {
"use strict";

function setUpUsers(rst) {
    const primaryAdminDB = rst.getPrimary().getDB("admin");
    assert.commandWorked(
        primaryAdminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert.eq(1, primaryAdminDB.auth("admin", "admin"));

    assert.commandWorked(primaryAdminDB.getSiblingDB("test").runCommand(
        {createUser: "NotTrusted", pwd: "pwd", roles: ["readWrite"]}));
    primaryAdminDB.logout();

    authutil.asCluster(rst.nodes, "jstests/libs/key1", () => {
        rst.awaitLastOpCommitted();
    });
}

// Start with auth enabled so cluster times are validated.
const rst = new ReplSetTest({nodes: 2, keyFile: "jstests/libs/key1"});
rst.startSet();
rst.initiate();

setUpUsers(rst);

const secondaryAdminDB = new Mongo(rst.getSecondary().host).getDB("admin");
secondaryAdminDB.auth("admin", "admin");

const secondaryTestDB = rst.getSecondary().getDB("test");
secondaryTestDB.auth("NotTrusted", "pwd");

// Cluster time should be gossipped in the steady state. This requires the secondary to have cached
// the cluster time keys which requires reading from a majority snapshot and may not have happened
// immediately after replica set startup, so retry until times are returned.
let res, validClusterTimeMetadata;
assert.soonNoExcept(() => {
    res = assert.commandWorked(secondaryTestDB.runCommand({find: "foo", filter: {}}));
    assert.hasFields(res, ["$clusterTime", "operationTime"]);
    validClusterTimeMetadata = Object.assign({}, res.$clusterTime);
    return true;
});

// After entering maintenance mode, cluster time should no longer be gossipped, in or out.
assert.commandWorked(secondaryAdminDB.adminCommand({replSetMaintenance: 1}));

// The find should fail because the node is unreadable.
res = assert.commandFailedWithCode(secondaryTestDB.runCommand({find: "foo", filter: {}}),
                                   ErrorCodes.NotPrimaryOrSecondary);
assert(!res.hasOwnProperty("$clusterTime"), tojson(res));
assert(!res.hasOwnProperty("operationTime"), tojson(res));

// A request with $clusterTime should be ignored. This is verified by sending an invalid
// $clusterTime to emulate situations where valid cluster times would be unable to be verified, e.g.
// when the signing keys have not been cached but cannot be read from admin.system.keys because the
// node is in an unreadable state.
const invalidClusterTimeMetadata = Object.assign(
    validClusterTimeMetadata,
    {clusterTime: new Timestamp(validClusterTimeMetadata.clusterTime.getTime() + 100, 0)});
res = assert.commandWorked(
    secondaryTestDB.runCommand({hello: 1, $clusterTime: invalidClusterTimeMetadata}));

assert.commandWorked(secondaryAdminDB.adminCommand({replSetMaintenance: 0}));

res = assert.commandWorked(secondaryTestDB.runCommand({find: "foo", filter: {}}));
assert.hasFields(res, ["$clusterTime", "operationTime"]);

// A request with invalid cluster time metadata should now be rejected.
assert.commandFailedWithCode(
    secondaryTestDB.runCommand({hello: 1, $clusterTime: invalidClusterTimeMetadata}),
    ErrorCodes.TimeProofMismatch);

secondaryAdminDB.logout();
secondaryTestDB.logout();

rst.stopSet();
})();
