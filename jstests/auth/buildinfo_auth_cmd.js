/**
 * Tests that buildInfo command will fail if the connection is not authenticated.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const db = conn.getDB('admin');

    // Create a user without any roles, and authenticate. This should be enough to call buildInfo
    // since it only checks the connection is authenticated.
    db.createUser({user: 'admin', pwd: 'pwd', roles: []});
    assert(db.auth('admin', 'pwd'));
    assert.commandWorked(db.runCommand({buildinfo: 1}));
    db.logout();

    // Command should fail if unauthenticated.
    assert.commandFailedWithCode(db.runCommand({buildinfo: 1}), ErrorCodes.Unauthorized);
}

// Test standalone.
const m = MongoRunner.runMongod({auth: ""});
runTest(m);
MongoRunner.stopMongod(m);

// Test sharded.
const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();
