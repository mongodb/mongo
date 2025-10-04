/**
 * Tests that mongos and the shard discover changes to the shard's replica set membership.
 *
 * @tags: [
 *   # TODO (SERVER-85629): Re-enable this test once redness is resolved in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 *   requires_fcv_80
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";

let five_minutes = 5 * 60 * 1000;

let numRSHosts = function () {
    let result = assert.commandWorked(rsObj.nodes[0].adminCommand({hello: 1}));
    return result.hosts.length + result.passives.length;
};

let numMongosHosts = function () {
    let commandResult = assert.commandWorked(mongos.adminCommand("connPoolStats"));
    let result = commandResult.replicaSets[rsObj.name];
    return result.hosts.length;
};

let configServerURL = function () {
    let result = config.shards.find().toArray()[0];
    return result.host;
};

let checkNumHosts = function (expectedNumHosts) {
    jsTest.log("Waiting for the shard to discover that it now has " + expectedNumHosts + " hosts.");
    let numHostsSeenByShard;

    // Use a high timeout (5 minutes) because replica set refreshes are only done every 30
    // seconds.
    assert.soon(
        function () {
            numHostsSeenByShard = numRSHosts();
            return numHostsSeenByShard === expectedNumHosts;
        },
        function () {
            return "Expected shard to see " + expectedNumHosts + " hosts but found " + numHostsSeenByShard;
        },
        five_minutes,
    );

    jsTest.log("Waiting for the mongos to discover that the shard now has " + expectedNumHosts + " hosts.");
    let numHostsSeenByMongos;

    // Use a high timeout (5 minutes) because replica set refreshes are only done every 30
    // seconds.
    assert.soon(
        function () {
            numHostsSeenByMongos = numMongosHosts();
            return numHostsSeenByMongos === expectedNumHosts;
        },
        function () {
            return "Expected mongos to see " + expectedNumHosts + " hosts on shard but found " + numHostsSeenByMongos;
        },
        five_minutes,
    );
};

let st = new ShardingTest({
    name: "mongos_no_replica_set_refresh",
    shards: 1,
    mongos: 1,
    other: {
        rs0: {
            nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        },
    },
});

var rsObj = st.rs0;
assert.commandWorked(
    rsObj.nodes[0].adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: 60 * 1000,
    }),
    "node 0 " + rsObj.nodes[0].host + " failed to become primary",
);

var mongos = st.s;
var config = mongos.getDB("config");

printjson(mongos.getCollection("foo.bar").findOne());

jsTestLog("Removing a node from the shard's replica set.");

let rsConfig = rsObj.getReplSetConfigFromNode(0);

let removedNode = rsConfig.members.pop();
rsConfig.version++;
reconfig(rsObj, rsConfig);

// Wait for the election round to complete
rsObj.getPrimary();

checkNumHosts(rsConfig.members.length);

jsTest.log("Waiting for config.shards to reflect that " + removedNode.host + " has been removed.");
assert.soon(
    function () {
        return configServerURL().indexOf(removedNode.host) < 0;
    },
    function () {
        return removedNode.host + " was removed from " + rsObj.name + ", but is still seen in config.shards";
    },
);

jsTestLog("Adding the node back to the shard's replica set.");

config.shards.update({_id: rsObj.name}, {$set: {host: rsObj.name + "/" + rsObj.nodes[0].host}});
printjson(config.shards.find().toArray());

rsConfig.members.push(removedNode);
rsConfig.version++;
reconfig(rsObj, rsConfig);

checkNumHosts(rsConfig.members.length);

jsTest.log("Waiting for config.shards to reflect that " + removedNode.host + " has been re-added.");
assert.soon(
    function () {
        return configServerURL().indexOf(removedNode.host) >= 0;
    },
    function () {
        return removedNode.host + " was re-added to " + rsObj.name + ", but is not seen in config.shards";
    },
);

st.stop({parallelSupported: false});
