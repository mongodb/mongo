/*
 * This is a regression test for SERVER-16189, to make sure a replica set with both current and
 * prior version nodes can be initialized from the prior version node.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "initialize_from_old";
// Test old version with both "last-lts" and "last-continuous".
for (let oldVersion of ["last-lts", "last-continuous"]) {
    jsTestLog("Testing replSetInitiate with " + oldVersion);
    let newVersion = "latest";
    let nodes = {
        n0: {binVersion: oldVersion},
        n1: {binVersion: newVersion},
        n2: {binVersion: newVersion},
    };
    let rst = new ReplSetTest({nodes: nodes, name: name});
    let conns = rst.startSet();
    let oldNode = conns[0];
    let config = rst.getReplSetConfig();
    let response = oldNode.getDB("admin").runCommand({replSetInitiate: config});
    assert.commandWorked(response);
    // Wait for secondaries to finish their initial sync before shutting down the cluster.
    rst.awaitSecondaryNodes();
    rst.stopSet();
}
