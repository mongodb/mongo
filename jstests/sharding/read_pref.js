/**
 * Integration test for read preference and tagging.
 */

// This test shuts down a shard's node and because of this consistency checking
// cannot be performed on that node, which causes the consistency checker to fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let PRI_TAG = {dc: "ny"};
let SEC_TAGS = [
    {dc: "sf", s: "1"},
    {dc: "ma", s: "2"},
    {dc: "eu", s: "3"},
    {dc: "jp", s: "4"},
];
let NODES = SEC_TAGS.length + 1;

let doTest = function () {
    let st = new ShardingTest({shards: {rs0: {nodes: NODES, oplogSize: 10, useHostName: true}}});
    let replTest = st.rs0;
    let primaryNode = replTest.getPrimary();

    let setupConf = function () {
        let replConf = primaryNode.getDB("local").system.replset.findOne();
        replConf.version = (replConf.version || 0) + 1;

        let secIdx = 0;
        for (let x = 0; x < NODES; x++) {
            let node = replConf.members[x];

            if (node.host == primaryNode.name) {
                node.tags = PRI_TAG;
            } else {
                node.tags = SEC_TAGS[secIdx++];
                node.priority = 0;
            }
        }

        try {
            primaryNode.getDB("admin").runCommand({replSetReconfig: replConf});
        } catch (x) {
            jsTest.log("Exception expected because reconfiguring would close all conn, got " + x);
        }

        return replConf;
    };

    let checkTag = function (nodeToCheck, tag) {
        for (let idx = 0; idx < NODES; idx++) {
            let node = replConf.members[idx];

            if (node.host == nodeToCheck) {
                jsTest.log("node[" + node.host + "], Tag: " + tojson(node["tags"]));
                jsTest.log("tagToCheck: " + tojson(tag));

                let nodeTag = node["tags"];

                for (let key in tag) {
                    assert.eq(tag[key], nodeTag[key]);
                }

                return;
            }
        }

        assert(false, "node " + nodeToCheck + " not part of config!");
    };

    var replConf = setupConf();

    let conn = st.s;

    // Wait until the ReplicaSetMonitor refreshes its view and see the tags
    let replConfig = replTest.getReplSetConfigFromNode();
    replConfig.members.forEach(function (node) {
        let nodeConn = new Mongo(node.host);
        awaitRSClientHosts(conn, nodeConn, {ok: true, tags: node.tags}, replTest);
    });
    replTest.awaitReplication();

    jsTest.log("New rs config: " + tojson(primaryNode.getDB("local").system.replset.findOne()));
    jsTest.log("connpool: " + tojson(conn.getDB("admin").runCommand({connPoolStats: 1})));

    let coll = conn.getDB("test").user;

    assert.soon(function () {
        let res = coll.insert({x: 1}, {writeConcern: {w: NODES}});
        if (!res.hasWriteError()) {
            return true;
        }

        let err = res.getWriteError().errmsg;
        // Transient transport errors may be expected b/c of the replSetReconfig
        if (err.indexOf("transport error") == -1) {
            throw err;
        }
        return false;
    });

    let getExplain = function (readPrefMode, readPrefTags) {
        return coll.find().readPref(readPrefMode, readPrefTags).explain("executionStats");
    };

    let getExplainServer = function (explain) {
        assert.eq("SINGLE_SHARD", explain.queryPlanner.winningPlan.stage);
        let serverInfo = explain.queryPlanner.winningPlan.shards[0].serverInfo;
        return serverInfo.host + ":" + serverInfo.port.toString();
    };

    // Read pref should work without secondaryOk
    let explain = getExplain("secondary");
    let explainServer = getExplainServer(explain);
    assert.neq(primaryNode.name, explainServer);

    conn.setSecondaryOk();

    // It should also work with secondaryOk
    explain = getExplain("secondary");
    explainServer = getExplainServer(explain);
    assert.neq(primaryNode.name, explainServer);

    // Check that $readPreference does not influence the actual query
    assert.eq(1, explain.executionStats.nReturned);

    explain = getExplain("secondaryPreferred", [{s: "2"}]);
    explainServer = getExplainServer(explain);
    checkTag(explainServer, {s: "2"});
    assert.eq(1, explain.executionStats.nReturned);

    // Cannot use tags with primaryOnly
    assert.throws(function () {
        getExplain("primary", [{s: "2"}]);
    });

    // Ok to use empty tags on primaryOnly
    explain = getExplain("primary", [{}]);
    explainServer = getExplainServer(explain);
    assert.eq(primaryNode.name, explainServer);

    explain = getExplain("primary", []);
    explainServer = getExplainServer(explain);
    assert.eq(primaryNode.name, explainServer);

    // Check that mongos will try the next tag if nothing matches the first
    explain = getExplain("secondary", [{z: "3"}, {dc: "jp"}]);
    explainServer = getExplainServer(explain);
    checkTag(explainServer, {dc: "jp"});
    assert.eq(1, explain.executionStats.nReturned);

    // Check that mongos will fallback to primary if none of tags given matches
    explain = getExplain("secondaryPreferred", [{z: "3"}, {dc: "ph"}]);
    explainServer = getExplainServer(explain);
    // Call getPrimary again since the primary could have changed after the restart.
    assert.eq(replTest.getPrimary().name, explainServer);
    assert.eq(1, explain.executionStats.nReturned);

    // TODO (SERVER-83433): Add back the test coverage for running db hash check and validation
    // on replica set that is fsync locked and has replica set endpoint enabled.
    const stopOpts = {skipValidation: replTest.isReplicaSetEndpointActive()};

    // Kill all members except one
    let stoppedNodes = [];
    for (let x = 0; x < NODES - 1; x++) {
        replTest.stop(x, null, stopOpts);
        stoppedNodes.push(replTest.nodes[x]);
    }

    // Wait for ReplicaSetMonitor to realize nodes are down
    awaitRSClientHosts(conn, stoppedNodes, {ok: false}, replTest.name);

    // Wait for the last node to be in steady state -> secondary (not recovering)
    let lastNode = replTest.nodes[NODES - 1];
    awaitRSClientHosts(conn, lastNode, {ok: true, secondary: true}, replTest.name);

    jsTest.log("connpool: " + tojson(conn.getDB("admin").runCommand({connPoolStats: 1})));

    // Test to make sure that connection is ok, in prep for priOnly test
    explain = getExplain("nearest");
    explainServer = getExplainServer(explain);
    assert.eq(explainServer, replTest.nodes[NODES - 1].name);
    assert.eq(1, explain.executionStats.nReturned);

    // Should assert if request with priOnly but no primary
    assert.throws(function () {
        getExplain("primary");
    });

    st.stop(stopOpts);
};

doTest();
