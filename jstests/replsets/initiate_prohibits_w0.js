/*
 * Test that replSetInitiate prohibits w:0 in getLastErrorDefaults,
 * SERVER-13055.
 * @tags: [multiversion_incompatible]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let InvalidReplicaSetConfig = 93;

let replTest = new ReplSetTest({name: "prohibit_w0", nodes: 1});
let nodes = replTest.nodeList();
let conns = replTest.startSet();
let admin = conns[0].getDB("admin");

function testInitiate(gleDefaults) {
    let conf = replTest.getReplSetConfig();
    jsTestLog("conf");
    printjson(conf);
    conf.settings = gleDefaults;

    let response = admin.runCommand({replSetInitiate: conf});
    assert.commandFailedWithCode(response, InvalidReplicaSetConfig);
}

/*
 * Try to initiate with w: 0 in getLastErrorDefaults.
 */
testInitiate({getLastErrorDefaults: {w: 0}});

/*
 * Try to initiate with w: 0 and other options in getLastErrorDefaults.
 */
testInitiate({getLastErrorDefaults: {w: 0, j: false, wtimeout: 100, fsync: true}});

replTest.stopSet();
