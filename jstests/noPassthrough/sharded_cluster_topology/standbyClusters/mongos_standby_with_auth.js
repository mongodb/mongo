/**
 * Tests that a mongoS can start up pointing to a standby config server.
 *
 * @tags: [
 *   # The StandbyClusterTestFixture restarts nodes and expects persisted metadata to be preserved.
 *   requires_persistence,
 * ]
 */

import {StandbyClusterTestFixture} from "jstests/noPassthrough/libs/sharded_cluster_topology/standby_cluster_test_fixture.js";

const KEY_FILE = "jstests/libs/key1";
const ADMIN_USER = "admin";
const ADMIN_PWD = "pwd";

const fixture = new StandbyClusterTestFixture({
    name: "mongos_start_with_standby_config",
    shards: 1,
    rs: {nodes: 1},
    config: {nodes: 1},
    keyFile: KEY_FILE,
});

// Create an admin user and authenticate before running any operations.
fixture.st.s.getDB("admin").createUser({user: ADMIN_USER, pwd: ADMIN_PWD, roles: ["root"]});
fixture.st.s.getDB("admin").auth(ADMIN_USER, ADMIN_PWD);

// Create both a sharded and unsharded collection to ensure that the catalog cache is correctly
// disabled.
assert.commandWorked(fixture.st.s.getDB("unshardedDB").getCollection("test").insertOne({x: 1}));
assert.commandWorked(fixture.st.s.getDB("shardedDB").getCollection("test").insertOne({x: 1}));
assert.commandWorked(fixture.st.s.adminCommand({shardCollection: "shardedDB.test", key: {_id: 1}}));

fixture.transitionToStandby();

const mongos = MongoRunner.runMongos({
    configdb: fixture.standbyRS.getURL(),
    configOnly: "",
    keyFile: KEY_FILE,
});
assert.neq(null, mongos, "mongoS failed to start up against standby config server");

// Authenticate on the new mongos before issuing commands.
mongos.getDB("admin").auth(ADMIN_USER, ADMIN_PWD);

assert.commandWorked(mongos.adminCommand({hello: 1}));

// Test that operations against both sharded and unsharded collections fail due to configOnly mode.
assert.commandFailedWithCode(mongos.getDB("unshardedDB").runCommand({find: "test"}), 12319007);
assert.commandFailedWithCode(mongos.getDB("shardedDB").runCommand({find: "test"}), 12319007);

MongoRunner.stopMongos(mongos);
fixture.teardown();
