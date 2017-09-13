/*
 * Tests various combinations of the configsvr field in replica set configurations and the
 * command line options that control whether a node can function as a member of a CSRS.
 *
 * This test requires mmapv1.
 * @tags: [requires_mmapv1]
 */

function expectState(rst, state) {
    assert.soon(function() {
        var status = rst.status();
        if (status.myState != state) {
            print("Waiting for state " + state + " in replSetGetStatus output: " + tojson(status));
        }
        return status.myState == state;
    });
}

(function() {
    "use strict";

    (function() {
        // Test that node with --configsvr cmd line and configsvr in replset config goes
        // into REMOVED state if storage engine is not WiredTiger
        jsTestLog("configsvr in rs config and --configsvr cmd line, but mmapv1");
        var rst = new ReplSetTest({
            name: "configrs3",
            nodes: 1,
            nodeOptions: {configsvr: "", journal: "", storageEngine: "mmapv1"}
        });

        rst.startSet();
        var conf = rst.getReplSetConfig();
        conf.configsvr = true;
        try {
            rst.nodes[0].adminCommand({replSetInitiate: conf});
        } catch (e) {
            // expected since we close all connections after going into REMOVED
        }
        expectState(rst, ReplSetTest.State.REMOVED);
        rst.stopSet();
    })();

    (function() {
        // Test that node with --configsvr cmd line and configsvr in replset config and using
        // wiredTiger
        // does NOT go into REMOVED state.
        jsTestLog("configsvr in rs config and --configsvr cmd line, normal case");
        var rst = new ReplSetTest({
            name: "configrs5",
            nodes: 1,
            nodeOptions: {configsvr: "", journal: "", storageEngine: "wiredTiger"}
        });

        rst.startSet();
        var conf = rst.getReplSetConfig();
        conf.configsvr = true;
        assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: conf}));

        rst.getPrimary();
        expectState(rst, ReplSetTest.State.PRIMARY);

        var conf = rst.getPrimary().getDB('local').system.replset.findOne();
        assert(conf.configsvr, tojson(conf));

        rst.stopSet();
    })();

    (function() {
        // Test that node with --configsvr cmd line and initiated with an empty replset config
        // will result in configsvr:true getting automatically added to the config (SERVER-20247).
        jsTestLog("--configsvr cmd line, empty config to replSetInitiate");
        var rst = new ReplSetTest({
            name: "configrs6",
            nodes: 1,
            nodeOptions: {configsvr: "", journal: "", storageEngine: "wiredTiger"}
        });

        rst.startSet();
        assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: 1}));

        rst.getPrimary();
        expectState(rst, ReplSetTest.State.PRIMARY);
        rst.stopSet();
    })();

    (function() {
        // Test that a set initialized without --configsvr but then restarted with --configsvr will
        // fail to start up and won't automatically add "configsvr" to the replset config
        // (SERVER-21236).
        jsTestLog("set initiated without configsvr, restarted adding --configsvr cmd line");
        var rst = new ReplSetTest(
            {name: "configrs7", nodes: 1, nodeOptions: {journal: "", storageEngine: "wiredTiger"}});

        rst.startSet();
        var conf = rst.getReplSetConfig();
        assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: conf}));

        rst.getPrimary();
        expectState(rst, ReplSetTest.State.PRIMARY);

        var node = rst.nodes[0];
        var options = node.savedOptions;
        options.configsvr = "";
        options.noCleanData = true;
        options.waitForConnect = false;

        MongoRunner.stopMongod(node);

        var mongod = MongoRunner.runMongod(options);
        var exitCode = waitProgram(mongod.pid);
        assert.eq(
            MongoRunner.EXIT_ABRUPT, exitCode, "Mongod should have failed to start, but didn't");

        rst.stopSet();
    })();

    (function() {
        // Test that a set initialized with --configsvr but then restarted without --configsvr will
        // fail to start up.
        jsTestLog("set initiated with configsvr, restarted without --configsvr cmd line");
        var rst = new ReplSetTest({
            name: "configrs8",
            nodes: 1,
            nodeOptions: {configsvr: "", journal: "", storageEngine: "wiredTiger"}
        });

        rst.startSet();
        var conf = rst.getReplSetConfig();
        conf.configsvr = true;
        assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: conf}));

        rst.getPrimary();
        expectState(rst, ReplSetTest.State.PRIMARY);

        var node = rst.nodes[0];
        var options = node.savedOptions;
        delete options.configsvr;
        options.noCleanData = true;
        options.waitForConnect = false;

        MongoRunner.stopMongod(node);

        var mongod = MongoRunner.runMongod(options);
        var exitCode = waitProgram(mongod.pid);
        assert.eq(
            MongoRunner.EXIT_ABRUPT, exitCode, "Mongod should have failed to start, but didn't");

        rst.stopSet();
    })();

})();
