/*
 * Tests to validate set up of the ShardingTest enabling the embedded router and that it does report
 * the router port to `config.mongos`.
 *
 * @tags: [
 *    featureFlagRouterPort,
 *    requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    nodes: 1,
    configShard: true,
    embeddedRouter: true,
});

const routerConn = st.s;
let routerHelloResponse = assert.commandWorked(routerConn.adminCommand({hello: 1}));
let routerServerStatus = assert.commandWorked(routerConn.adminCommand({serverStatus: 1}));

// Hello on the router port should report that it is a router:
assert.eq(routerHelloResponse.msg, "isdbgrid");
assert(!routerHelloResponse.hasOwnProperty("setName"));
// But it should be a mongod process!
if (!_isWindows()) {
    assert.eq(routerServerStatus.process, "mongod");
} else {
    assert.eq(routerServerStatus.process, "mongod.exe");
}

const shardConn = st.rs0.getPrimary();
let shardHelloResponse = assert.commandWorked(shardConn.adminCommand({hello: 1}));

// Hello on the shard should report it is part of a replica set:
assert(shardHelloResponse.hasOwnProperty("setName"));
assert(shardHelloResponse.hasOwnProperty("secondary"));

// Check that we can connect to the router port of the mongod/primary.
let primaryNodeRouterConn = new Mongo(shardConn.routerHost);
let primaryRouterHelloResponse =
    assert.commandWorked(primaryNodeRouterConn.adminCommand({hello: 1}));
// And that it acts like a router
assert.eq(primaryRouterHelloResponse.msg, "isdbgrid");
assert(!primaryRouterHelloResponse.hasOwnProperty("setName"));

// Check that the shard is reporting the router port to config.mongos as embedded.
const configMongosDB = routerConn.getDB("config").mongos;
assert.soon(() => configMongosDB.exists());
const res = configMongosDB.find().toArray();
assert.eq(1, res.length);
assert(res[0].embeddedRouter);

st.stop();
