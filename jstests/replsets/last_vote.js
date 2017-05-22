// Tests that the last vote document is stored during elections and that it is loaded and used on
// startup.
//
// The test first runs a few elections and checks that the lastVote document is set correctly
// after each one.
//
// The test then restarts one node as a standalone, changes its last vote doc, and stops the
// other node. It then restarts the first node as a replicaset and manually runs
// replSetRequestVotes commands against it and checks that its response is correct.
//
// @tags: [requires_persistence]

(function() {
    "use strict";
    load("jstests/replsets/rslib.js");  // For getLatestOp()

    var name = "last_vote";
    var rst = new ReplSetTest({
        name: name,
        nodes: 2,
    });
    rst.startSet();

    // Lower the election timeout to make the test run faster since it waits for multiple elections.
    var conf = rst.getReplSetConfig();
    conf.settings = {
        electionTimeoutMillis: 6000,
    };
    rst.initiate(conf);

    const lastVoteNS = 'local.replset.election';

    function getLastVoteDoc(conn) {
        assert.eq(
            conn.getCollection(lastVoteNS).find().itcount(), 1, 'last vote should be singleton');
        return conn.getCollection(lastVoteNS).findOne();
    }

    function setLastVoteDoc(conn, term, candidate) {
        var newLastVote = {term: term, candidateIndex: rst.getNodeId(candidate)};
        return assert.writeOK(conn.getCollection(lastVoteNS).update({}, newLastVote));
    }

    function assertNodeHasLastVote(node, term, candidate) {
        var lastVoteDoc = getLastVoteDoc(node);
        assert.eq(lastVoteDoc.term, term, node.host + " had wrong last vote term.");
        assert.eq(lastVoteDoc.candidateIndex,
                  rst.getNodeId(candidate),
                  node.host + " had wrong last vote candidate.");
    }

    function assertCurrentTerm(node, term) {
        var stat = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
        assert.eq(stat.term, term, "Term changed when it should not have");
    }

    jsTestLog("Test that last vote is set on successive elections");

    // Run a few successive elections, alternating who becomes primary.
    var numElections = 3;
    for (var i = 0; i < numElections; i++) {
        var primary = rst.getPrimary();
        var secondary = rst.getSecondary();
        var term = getLatestOp(primary).t;

        // SERVER-20844 ReplSetTest starts up a single node replica set then reconfigures to the
        // correct size, so secondaries didn't vote in the first election.
        if (i > 0) {
            jsTestLog("Last vote should have term: " + term + " and candidate: " + primary.host +
                      ", index: " + rst.getNodeId(primary));
            rst.nodes.forEach(function(node) {
                assertNodeHasLastVote(node, term, primary);
            });
        }
        assert.throws(function() {
            primary.adminCommand({replSetStepDown: 60 * 10, force: true});
        });

        // Make sure a new primary has been established.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
        rst.waitForState(secondary, ReplSetTest.State.PRIMARY);

        // Reset election timeout for the old primary.
        assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
    }

    var term = getLatestOp(rst.getPrimary()).t + 100;

    jsTestLog("Test that last vote is loaded on startup");

    // Ensure that all ops are replicated before stepping up node 1.
    rst.awaitReplication();

    // We cannot reconfig node 0 to have priority 0 if it is currently the primary,
    // so we make sure node 1 is primary.
    jsTestLog("Stepping up node 1");
    rst.stepUp(rst.nodes[1]);

    jsTestLog("Reconfiguring cluster to make node 0 unelectable so it stays SECONDARY on restart");
    conf = rst.getReplSetConfigFromNode();
    conf.version++;
    conf.members[0].priority = 0;
    reconfig(rst, conf);
    rst.awaitNodesAgreeOnConfigVersion();

    jsTestLog("Restarting node 0 as a standalone");
    var node0 = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
    jsTestLog("Stopping node 1");
    rst.stop(1);  // Stop node 1 so that node 0 controls the term by itself.
    jsTestLog("Setting the lastVote on node 0 to term: " + term + " candidate: " +
              rst.nodes[0].host + ", index: 0");
    setLastVoteDoc(node0, term, rst.nodes[0]);

    jsTestLog("Restarting node 0 in replica set mode");
    node0 = rst.restart(0);  // Restart in replSet mode again.
    assert.soonNoExcept(function() {
        assertCurrentTerm(node0, term);
        return true;
    });

    jsTestLog("Manually sending node 0 a dryRun replSetRequestVotes command, " +
              "expecting failure in old term");
    var response = assert.commandWorked(node0.adminCommand({
        replSetRequestVotes: 1,
        setName: name,
        dryRun: true,
        term: term - 1,
        candidateIndex: 1,
        configVersion: conf.version,
        lastCommittedOp: getLatestOp(node0)
    }));
    assert.eq(response.term,
              term,
              "replSetRequestVotes response had the wrong term: " + tojson(response));
    assert(!response.voteGranted,
           "node granted vote in term before last vote doc: " + tojson(response));
    assertNodeHasLastVote(node0, term, rst.nodes[0]);
    assertCurrentTerm(node0, term);

    jsTestLog("Manually sending node 0 a dryRun replSetRequestVotes command in same term, " +
              "expecting success but no recording of lastVote");
    response = assert.commandWorked(node0.adminCommand({
        replSetRequestVotes: 1,
        setName: name,
        dryRun: true,
        term: term,
        candidateIndex: 1,
        configVersion: conf.version,
        lastCommittedOp: getLatestOp(node0)
    }));
    assert.eq(response.term,
              term,
              "replSetRequestVotes response had the wrong term: " + tojson(response));
    assert(response.voteGranted,
           "node failed to grant dryRun vote in term equal to last vote doc: " + tojson(response));
    assert.eq(response.reason,
              "",
              "replSetRequestVotes response had the wrong reason: " + tojson(response));
    assertNodeHasLastVote(node0, term, rst.nodes[0]);
    assertCurrentTerm(node0, term);

    jsTestLog(
        "Manually sending node 0 a replSetRequestVotes command, expecting failure in same term");
    response = assert.commandWorked(node0.adminCommand({
        replSetRequestVotes: 1,
        setName: name,
        dryRun: false,
        term: term,
        candidateIndex: 1,
        configVersion: conf.version,
        lastCommittedOp: getLatestOp(node0)
    }));
    assert.eq(response.term,
              term,
              "replSetRequestVotes response had the wrong term: " + tojson(response));
    assert(!response.voteGranted,
           "node granted vote in term of last vote doc: " + tojson(response));
    assertNodeHasLastVote(node0, term, rst.nodes[0]);
    assertCurrentTerm(node0, term);

    jsTestLog("Manually sending node 0 a replSetRequestVotes command, " +
              "expecting success with a recording of the new lastVote");
    response = assert.commandWorked(node0.adminCommand({
        replSetRequestVotes: 1,
        setName: name,
        dryRun: false,
        term: term + 1,
        candidateIndex: 1,
        configVersion: conf.version,
        lastCommittedOp: getLatestOp(node0)
    }));
    assert.eq(response.term,
              term + 1,
              "replSetRequestVotes response had the wrong term: " + tojson(response));
    assert(response.voteGranted,
           "node failed to grant vote in term greater than last vote doc: " + tojson(response));
    assert.eq(response.reason,
              "",
              "replSetRequestVotes response had the wrong reason: " + tojson(response));
    assertNodeHasLastVote(node0, term + 1, rst.nodes[1]);
    assertCurrentTerm(node0, term + 1);

    jsTestLog("Manually sending node 0 a dryRun replSetRequestVotes command in future term, " +
              "expecting success but no recording of lastVote");
    response = assert.commandWorked(node0.adminCommand({
        replSetRequestVotes: 1,
        setName: name,
        dryRun: true,
        term: term + 2,
        candidateIndex: 1,
        configVersion: conf.version,
        lastCommittedOp: getLatestOp(node0)
    }));
    assert.eq(response.term,
              term + 2,
              "replSetRequestVotes response had the wrong term: " + tojson(response));
    assert(response.voteGranted,
           "node failed to grant vote in term greater than last vote doc: " + tojson(response));
    assert.eq(response.reason,
              "",
              "replSetRequestVotes response had the wrong reason: " + tojson(response));
    assertNodeHasLastVote(node0, term + 1, rst.nodes[1]);
    assertCurrentTerm(node0, term + 2);

    rst.stopSet();
})();
