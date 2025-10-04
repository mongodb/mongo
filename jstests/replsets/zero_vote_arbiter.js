/*
 * Test that replSetInitiate and replSetReconfig prohibit zero-vote arbiters,
 * SERVER-13627.
 *
 * @tags: [multiversion_incompatible]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let NewReplicaSetConfigurationIncompatible = 103;
let InvalidReplicaSetConfig = 93;

// Skip db hash check since replsets are assigned invalid configs.
TestData.skipCheckDBHashes = true;

/*
 * Create replica set with 3 nodes, add new node as 0-vote arbiter.
 */
(function addArbiterZeroVotes() {
    let replTest = new ReplSetTest({nodes: 3});
    replTest.startSet();
    replTest.initiate();

    let arbiterConn = replTest.add();
    let admin = replTest.getPrimary().getDB("admin");
    let conf = admin.runCommand({replSetGetConfig: 1}).config;
    conf.members.push({_id: 3, host: arbiterConn.host, arbiterOnly: true, votes: 0});
    conf.version++;

    jsTestLog("Add arbiter with zero votes:");
    let response = admin.runCommand({replSetReconfig: conf});
    assert.commandFailed(response);
    assert.eq(response.code, InvalidReplicaSetConfig);
    assert(/.*arbiter must vote.*/i.test(response.errmsg));

    replTest.stopSet();
})();

/*
 * Replica set with 4 nodes, 2 are arbiters. Reconfigure one with 0 votes.
 */
(function reconfigArbiterZeroVotes() {
    let replTest = new ReplSetTest({nodes: 4});
    replTest.startSet({setParameter: {allowMultipleArbiters: true}});
    let config = replTest.getReplSetConfig();
    config.members[2].arbiterOnly = true;
    config.members[3].arbiterOnly = true;
    replTest.initiate(config);

    let admin = replTest.getPrimary().getDB("admin");
    let conf = admin.runCommand({replSetGetConfig: 1}).config;

    jsTestLog("Reconfig arbiter with zero votes:");
    conf.members[3].votes = 0;
    conf.version++;
    let response = admin.runCommand({replSetReconfig: conf});
    printjson(response);
    assert.commandFailed(response);
    assert.eq(response.code, InvalidReplicaSetConfig);
    assert(/.*arbiter must vote.*/i.test(response.errmsg));

    replTest.stopSet();
})();

/*
 * replSetInitiate with a 0-vote arbiter.
 */
(function initiateArbiterZeroVotes() {
    let replTest = new ReplSetTest({nodes: 3});
    let conns = replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[2].arbiterOnly = true;
    config.members[2].votes = 0;

    let admin = conns[0].getDB("admin");

    jsTestLog("replSetInitiate with 0-vote arbiter:");
    let response = admin.runCommand({replSetInitiate: config});
    printjson(response);
    assert.commandFailed(response);

    // Test for SERVER-15838 wrong error from RS init with 0-vote arbiter.
    assert.eq(response.code, InvalidReplicaSetConfig);
    assert(/.*arbiter must vote.*/i.test(response.errmsg));

    replTest.stopSet();
})();

/*
 * Replica set with max number of voting nodes. Add a 0-vote arbiter.
 */
(function maxVoteEdgeAddArbiterZeroVotes() {
    let replTest = new ReplSetTest({nodes: 7});
    replTest.startSet();
    replTest.initiate();

    let arbiterConn = replTest.add();
    let admin = replTest.getPrimary().getDB("admin");
    let conf = admin.runCommand({replSetGetConfig: 1}).config;
    conf.members.push({_id: 7, host: arbiterConn.host, arbiterOnly: true, votes: 0});
    conf.version++;

    jsTestLog("Add arbiter with zero votes:");
    let response = admin.runCommand({replSetReconfig: conf});
    assert.commandFailed(response);
    assert.eq(response.code, InvalidReplicaSetConfig);
    assert(/.*arbiter must vote.*/i.test(response.errmsg));

    replTest.stopSet();
})();
