/**
 * In a 2 (or 3) node replica set, a new candidate should be able to overtake a current primary with
 * a single round of election votes. This is enabled by the ability of a current primary to both
 * step down *and* cast its vote for a new primary in a single step, in response to a vote request
 * from a higher term than its own. This test verifies that an old primary is able to do this
 * successfully.
 */
(function() {
    "use strict";

    let name = "primary_casts_vote_on_stepdown";
    let replTest = new ReplSetTest({name: name, nodes: 2});

    let nodes = replTest.startSet();
    replTest.initiate();

    // Make sure node 0 is initially primary, and then step up node 1 and make sure it is able to
    // become primary in one election, gathering the vote of node 0, who will be forced to step
    // down in the act of granting its vote to node 1.
    jsTestLog("Make sure node 0 (" + nodes[0] + ") is primary.");
    replTest.waitForState(nodes[0], ReplSetTest.State.PRIMARY);
    let res = assert.commandWorked(nodes[0].adminCommand("replSetGetStatus"));
    let firstPrimaryTerm = res.term;

    jsTestLog("Stepping up node 1 (" + nodes[1] + ").");
    replTest.stepUp(nodes[1]);
    replTest.waitForState(nodes[1], ReplSetTest.State.PRIMARY);
    // The election should have happened in a single attempt, so the term of the new primary should
    // be exactly 1 greater than the old primary.
    res = assert.commandWorked(nodes[1].adminCommand("replSetGetStatus"));
    assert.eq(firstPrimaryTerm + 1, res.term);

    replTest.stopSet();

})();
