/*
 * Tests various combinations of the configServer field in replica set configurations and the
 * command line options that control whether a node can function as a member of a CSRS.
 */

function expectState(rst, state) {
    assert.soon(function() {
                    var status = rst.status();
                    if (status.myState != state) {
                        print("Waiting for state " + state +
                              " in replSetGetStatus output: " + tojson(status));
                    }
                    return status.myState == state;
                });
}

(function() {
"use strict";

(function() {
// Test that node without --configsvr cmd line but with configServer in replset config goes
// into REMOVED state
jsTestLog("configServer in rs config, no --configsvr cmd line")
var rst = new ReplSetTest({name: "configrs1",
                           nodes: 1,
                           nodeOptions: {storageEngine: "wiredTiger"}});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.configServer = true;
try {
    rst.nodes[0].adminCommand({replSetInitiate: conf});
} catch (e) {
    // expected since we close all connections after going into REMOVED
}
expectState(rst, 10 /*REMOVED*/);
rst.stopSet();
})();


(function() {
// Test that node with --configsvr cmd line but without configServer in replset config goes
// into REMOVED state
jsTestLog("no configServer in rs config but --configsvr cmd line");
var rst = new ReplSetTest({name: "configrs2",
                           nodes: 1,
                           nodeOptions: {configsvr: "", storageEngine: "wiredTiger"}});

rst.startSet();
var conf = rst.getReplSetConfig();
try {
    rst.nodes[0].adminCommand({replSetInitiate: conf});
} catch (e) {
    // expected since we close all connections after going into REMOVED
}
expectState(rst, 10 /*REMOVED*/);
rst.stopSet();
})();

(function() {
// Test that node with --configsvr cmd line and configServer in replset config goes
// into REMOVED state if storage engine is not WiredTiger
jsTestLog("configServer in rs config and --configsvr cmd line, but mmapv1");
var rst = new ReplSetTest({name: "configrs3", nodes: 1, nodeOptions: {configsvr: "",
                                                                      storageEngine: "mmapv1"}});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.configServer = true;
try {
    rst.nodes[0].adminCommand({replSetInitiate: conf});
} catch (e) {
    // expected since we close all connections after going into REMOVED
}
expectState(rst, 10 /*REMOVED*/);
rst.stopSet();
})();

(function() {
// Test that node with --configsvr cmd line and configServer in replset config does NOT go
// into REMOVED state if storage engine is not WiredTiger but we're running in SCC mode
jsTestLog("configServer in rs config and --configsvr cmd line, but mmapv1 with configSvrMode=scc");
var rst = new ReplSetTest({name: "configrs4", nodes: 1, nodeOptions: {configsvr: "",
                                                                      storageEngine: "mmapv1",
                                                                      configsvrMode: "scc"}});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.configServer = true;
assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: conf}));

rst.getMaster();
expectState(rst, 1 /*PRIMARY*/);
rst.stopSet();
})();

(function() {
// Test that node with --configsvr cmd line and configServer in replset config and using wiredTiger
// does NOT go into REMOVED state.
jsTestLog("configServer in rs config and --configsvr cmd line, normal case");
var rst = new ReplSetTest({name: "configrs5",
                           nodes: 1,
                           nodeOptions: {configsvr: "", storageEngine: "wiredTiger"}});

rst.startSet();
var conf = rst.getReplSetConfig();
conf.configServer = true;
assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: conf}));

rst.getMaster();
expectState(rst, 1 /*PRIMARY*/);
rst.stopSet();
})();


})();
