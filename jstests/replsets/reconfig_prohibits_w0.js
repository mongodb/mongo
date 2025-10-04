/*
 * Test that replSetReconfig prohibits w:0 in getLastErrorDefaults,
 * SERVER-13055.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({name: "prohibit_w0", nodes: 1});
let nodes = replTest.nodeList();
let conns = replTest.startSet();
let admin = conns[0].getDB("admin");

replTest.initiate({_id: "prohibit_w0", members: [{_id: 0, host: nodes[0]}]});

function testReconfig(gleDefaults) {
    let conf = admin.runCommand({replSetGetConfig: 1}).config;
    jsTestLog("conf");
    printjson(conf);
    conf.settings = gleDefaults;
    conf.version++;

    let response = admin.runCommand({replSetReconfig: conf});
    assert.commandFailedWithCode(response, ErrorCodes.InvalidReplicaSetConfig);
}

/*
 * Try to reconfig with w: 0 in getLastErrorDefaults.
 */
testReconfig({getLastErrorDefaults: {w: 0}});

/*
 * Try to reconfig with w: 0 and other options in getLastErrorDefaults.
 */
testReconfig({getLastErrorDefaults: {w: 0, j: false, wtimeout: 100, fsync: true}});

replTest.stopSet();
