/**
 * Integration test for read preference and tagging. The more comprehensive unit test
 * can be found in dbtests/replica_set_monitor_test.cpp.
 */

load("jstests/replsets/rslib.js");

var PRI_TAG = {dc: 'ny'};
var SEC_TAGS = [{dc: 'sf', s: "1"}, {dc: 'ma', s: "2"}, {dc: 'eu', s: "3"}, {dc: 'jp', s: "4"}];
var NODES = SEC_TAGS.length + 1;

var doTest = function(useDollarQuerySyntax) {
    var st = new ShardingTest({shards: {rs0: {nodes: NODES, oplogSize: 10, useHostName: true}}});
    var replTest = st.rs0;
    var primaryNode = replTest.getPrimary();

    // The $-prefixed query syntax is only legal for compatibility mode reads, not for the
    // find/getMore commands.
    if (useDollarQuerySyntax && st.s.getDB("test").getMongo().useReadCommands()) {
        return;
    }

    var setupConf = function() {
        var replConf = primaryNode.getDB('local').system.replset.findOne();
        replConf.version = (replConf.version || 0) + 1;

        var secIdx = 0;
        for (var x = 0; x < NODES; x++) {
            var node = replConf.members[x];

            if (node.host == primaryNode.name) {
                node.tags = PRI_TAG;
            } else {
                node.tags = SEC_TAGS[secIdx++];
                node.priority = 0;
            }
        }

        try {
            primaryNode.getDB('admin').runCommand({replSetReconfig: replConf});
        } catch (x) {
            jsTest.log('Exception expected because reconfiguring would close all conn, got ' + x);
        }

        return replConf;
    };

    var checkTag = function(nodeToCheck, tag) {
        for (var idx = 0; idx < NODES; idx++) {
            var node = replConf.members[idx];

            if (node.host == nodeToCheck) {
                jsTest.log('node[' + node.host + '], Tag: ' + tojson(node['tags']));
                jsTest.log('tagToCheck: ' + tojson(tag));

                var nodeTag = node['tags'];

                for (var key in tag) {
                    assert.eq(tag[key], nodeTag[key]);
                }

                return;
            }
        }

        assert(false, 'node ' + nodeToCheck + ' not part of config!');
    };

    var replConf = setupConf();

    var conn = st.s;

    // Wait until the ReplicaSetMonitor refreshes its view and see the tags
    var replConfig = replTest.getReplSetConfigFromNode();
    replConfig.members.forEach(function(node) {
        var nodeConn = new Mongo(node.host);
        awaitRSClientHosts(conn, nodeConn, {ok: true, tags: node.tags}, replTest);
    });
    replTest.awaitReplication();

    jsTest.log('New rs config: ' + tojson(primaryNode.getDB('local').system.replset.findOne()));
    jsTest.log('connpool: ' + tojson(conn.getDB('admin').runCommand({connPoolStats: 1})));

    var coll = conn.getDB('test').user;

    assert.soon(function() {
        var res = coll.insert({x: 1}, {writeConcern: {w: NODES}});
        if (!res.hasWriteError()) {
            return true;
        }

        var err = res.getWriteError().errmsg;
        // Transient transport errors may be expected b/c of the replSetReconfig
        if (err.indexOf("transport error") == -1) {
            throw err;
        }
        return false;
    });

    var getExplain = function(readPrefMode, readPrefTags) {
        if (useDollarQuerySyntax) {
            var readPrefObj = {mode: readPrefMode};

            if (readPrefTags) {
                readPrefObj.tags = readPrefTags;
            }

            return coll.find({$query: {}, $readPreference: readPrefObj, $explain: true})
                .limit(-1)
                .next();
        } else {
            return coll.find().readPref(readPrefMode, readPrefTags).explain("executionStats");
        }
    };

    var getExplainServer = function(explain) {
        assert.eq("SINGLE_SHARD", explain.queryPlanner.winningPlan.stage);
        var serverInfo = explain.queryPlanner.winningPlan.shards[0].serverInfo;
        return serverInfo.host + ":" + serverInfo.port.toString();
    };

    // Read pref should work without slaveOk
    var explain = getExplain("secondary");
    var explainServer = getExplainServer(explain);
    assert.neq(primaryNode.name, explainServer);

    conn.setSlaveOk();

    // It should also work with slaveOk
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
    assert.throws(function() {
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

    // Kill all members except one
    var stoppedNodes = [];
    for (var x = 0; x < NODES - 1; x++) {
        replTest.stop(x);
        stoppedNodes.push(replTest.nodes[x]);
    }

    // Wait for ReplicaSetMonitor to realize nodes are down
    awaitRSClientHosts(conn, stoppedNodes, {ok: false}, replTest.name);

    // Wait for the last node to be in steady state -> secondary (not recovering)
    var lastNode = replTest.nodes[NODES - 1];
    awaitRSClientHosts(conn, lastNode, {ok: true, secondary: true}, replTest.name);

    jsTest.log('connpool: ' + tojson(conn.getDB('admin').runCommand({connPoolStats: 1})));

    // Test to make sure that connection is ok, in prep for priOnly test
    explain = getExplain("nearest");
    explainServer = getExplainServer(explain);
    assert.eq(explainServer, replTest.nodes[NODES - 1].name);
    assert.eq(1, explain.executionStats.nReturned);

    // Should assert if request with priOnly but no primary
    assert.throws(function() {
        getExplain("primary");
    });

    st.stop();
};

doTest(false);
doTest(true);
