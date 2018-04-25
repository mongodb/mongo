/**
 * serverStatus.metrics.repl.apply.batches.totalMillis is a cumulative measure of how much time a
 * node spends applying batches. This test checks that it includes the time spent waiting for
 * batches to finish, by comparing the time recorded after replicating a small and a large load.
 */

(function() {
    "use strict";

    // Gets the value of metrics.repl.apply.batches.totalMillis.
    function getTotalMillis(node) {
        return assert.commandWorked(node.adminCommand({serverStatus: 1}))
            .metrics.repl.apply.batches.totalMillis;
    }

    // Do a bulk insert of documents as: {{key: 0}, {key: 1}, {key: 2}, ... , {key: num-1}}
    function performBulkInsert(coll, key, num) {
        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < num; i++) {
            let doc = {};
            doc[key] = i;
            bulk.insert(doc);
        }
        assert.writeOK(bulk.execute());
        rst.awaitReplication();
    }

    let name = "apply_batches_totalMillis";
    let rst = new ReplSetTest({name: name, nodes: 2});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    let secondary = rst.getSecondary();
    let coll = primary.getDB(name)["foo"];

    // Perform an initial write on the system and ensure steady state.
    assert.writeOK(coll.insert({init: 0}));
    rst.awaitReplication();
    let baseTime = getTotalMillis(secondary);

    // Introduce a small load and wait for it to be replicated.
    performBulkInsert(coll, "small", 1000);

    // Record the time spent applying the small load.
    let timeAfterSmall = getTotalMillis(secondary);
    let deltaSmall = timeAfterSmall - baseTime;

    // Insert a significantly larger load.
    performBulkInsert(coll, "large", 20000);

    // Record the time spent applying the large load.
    let timeAfterLarge = getTotalMillis(secondary);
    let deltaLarge = timeAfterLarge - timeAfterSmall;

    jsTestLog(`Recorded deltas: {small: ${deltaSmall}ms, large: ${deltaLarge}ms}.`);

    // We should have recorded at least as much time on the second load as we did on the first.
    // This is a crude comparison that is only taken to check that the timer is used correctly.
    assert(deltaLarge >= deltaSmall, "Expected a higher net totalMillis for the larger load.");
    rst.stopSet();

})();
