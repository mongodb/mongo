/**
 * This test confirms the correct implementation of SERVER-23299. Due
 * to a bug fixed as SERVER-23274, replica set secondaries sometimes
 * fail to clear the "temp" flag on collections that have been renamed
 * as a means of marking the collection as permanent. This
 * particularly happens with collections created by the $out
 * aggregation stage, in versions 3.2.0 through 3.2.1.
 *
 * This test creates a collection using $out in 3.2.1, confirms that
 * the collection gets incorrectly deleted on secondary failover, then
 * confirms that if the user upgrades from 3.2.1 to latest before a
 * failover, then a collection produced with $out in 3.2.1 is not
 * deleted.
 */

load('./jstests/multiVersion/libs/multi_rs.js');

(function() {
    "use strict";

    var oldVersion = "3.2.1";
    var newVersion = "latest";

    var ex;
    function getTestDbForNode(node) {
        return node.getDB("test");
    }

    function expectTargetCollectionSize(node, sz) {
        assert.eq(sz, getTestDbForNode(node).target.find().itcount(), "On node " + node.host);
    }

    var rst = new ReplSetTest({nodes: [{}, {}, {arbiter: true}]});
    rst.startSet({binVersion: oldVersion});
    rst.initiate();
    var n0 = rst.getPrimary();
    assert.writeOK(getTestDbForNode(n0).source.insert({_id: 0}));

    jsTest.log("Performing aggregation to create target collection on " + n0.host);
    getTestDbForNode(n0).source.aggregate({$out: "target"});
    expectTargetCollectionSize(n0, 1);
    rst.awaitReplication();
    expectTargetCollectionSize(rst.getSecondary(), 1);

    jsTest.log("Stepping down " + n0.host);
    try {
        n0.adminCommand({replSetStepDown: 1200});
    } catch (ex) {
        assert(tojson(ex).includes(
                   "network error while attempting to run command 'replSetStepDown' on host"),
               tojson(ex));
    }
    var n1 = rst.getPrimary();

    jsTest.log("Confirming that SERVER-23274 is present in " + oldVersion);

    assert.neq(
        n1.host, n0.host, "Failed to switch primary to other node in set away from " + n1.host);
    expectTargetCollectionSize(n1, 0);
    rst.awaitReplication();
    expectTargetCollectionSize(n0, 0);

    jsTest.log("Performing aggregation to create target collection on " + n1.host);
    getTestDbForNode(n1).source.aggregate({$out: "target"});
    expectTargetCollectionSize(n1, 1);
    rst.awaitReplication();
    expectTargetCollectionSize(n0, 1);

    rst.upgradeSet({binVersion: "latest"});
    n1 = rst.getPrimary();
    n0 = rst.getSecondary();

    jsTest.log("Confirming that target collection remained after upgrade");
    expectTargetCollectionSize(n1, 1);
    expectTargetCollectionSize(n0, 1);

    jsTest.log("Confirming that target collection remained after switching primaries");
    jsTest.log("Stepping down " + n0.host);
    try {
        n1.adminCommand({replSetStepDown: 1200});
    } catch (ex) {
        assert(tojson(ex).includes(
                   "network error while attempting to run command 'replSetStepDown' on host"),
               tojson(ex));
    }
    var n0 = rst.getPrimary();
    assert.neq(
        n1.host, n0.host, "Failed to switch primary to other node in set away from " + n1.host);
    expectTargetCollectionSize(n1, 1);
    expectTargetCollectionSize(n0, 1);
}());
