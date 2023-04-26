/**
 * Test that a heartbeat reconfig propagated from the primary to a new secondary
 * successfully sets the default write concern on the secondary. To do this, we start either a PSS
 * or PSA replica set. We then add a fourth secondary to the replica set, and verify that it sets
 * its implicit default write concern correctly. We don't test cases with cluster-wide write concern
 * set, because then the secondary won't set its implicit default write concern from a heartbeat
 * reconfig.
 * @tags: [
 * ]
 */

(function() {
'use strict';

load("jstests/replsets/rslib.js");

function runTest(hasArbiter) {
    jsTestLog("Running test with hasArbiter: " + tojson(hasArbiter));

    let replSetNodes = 3;
    if (hasArbiter) {
        replSetNodes = [{}, {}, {arbiter: true}];
    }
    const rst = new ReplSetTest({
        nodes: replSetNodes,
    });

    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const newSecondary = rst.add();
    assert.soon(() => isConfigCommitted(primary));
    const config = rst.getReplSetConfigFromNode();

    config.members.push({_id: 3, host: newSecondary.host});
    config.version++;

    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    assert.soon(() => isConfigCommitted(primary));

    rst.waitForConfigReplication(primary);
    rst.awaitReplication();

    let res = assert.commandWorked(newSecondary.adminCommand({getDefaultRWConcern: 1}));
    // A PSS set will have a default write concern of {w: "majority"}. A PSA set will have a default
    // write concern of {w: 1}.
    if (hasArbiter) {
        assert(!res.defaultWriteConcern, tojson(res));
    } else {
        assert.eq(res.defaultWriteConcern, {w: "majority", wtimeout: 0}, tojson(res));
    }
    assert.eq(res.defaultWriteConcernSource, "implicit", tojson(res));
    rst.stopSet();
}

runTest(false /* hasArbiter */);
runTest(true /* hasArbiter */);
})();
