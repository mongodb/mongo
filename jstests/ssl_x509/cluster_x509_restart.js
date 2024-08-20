/**
 * This test does a simple restart of a replica set with X.509 cluster auth.
 *
 * @tags: [requires_persistence, requires_replication]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 3,
    waitForKeys: false,
    nodeOptions: {
        tlsMode: "requireTLS",
        clusterAuthMode: "x509",
        keyFile: "jstests/libs/key1",
        tlsCertificateKeyFile: "jstests/libs/server.pem",
        tlsCAFile: "jstests/libs/ca.pem",
        tlsAllowInvalidHostnames: ""
    }
});
rst.startSet();

rst.initiate();

rst.awaitReplication(3000);

// Create a user to login as when auth is enabled later
rst.getPrimary().getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']}, {w: 3});

let primary = rst.getPrimary();

assert.commandWorked(primary.getDB("admin").runCommand({hello: 1}));
assert.commandWorked(primary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));

rst.stopSet();