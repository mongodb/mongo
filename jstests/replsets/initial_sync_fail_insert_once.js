/**
 * Tests that initial sync can complete after a failed insert to a cloned collection.
 * The failpoint may fail once or twice depending on the order of the results of listCollection,
 * so we allow initial sync to retry 3 times.
 */

(function() {
    var name = 'initial_sync_fail_insert_once';
    var replSet = new ReplSetTest(
        {name: name, nodes: 2, nodeOptions: {setParameter: "numInitialSyncAttempts=3"}});

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, x: 1}, {writeConcern: {w: 2}}));

    jsTest.log("Enabling Failpoint failCollectionInserts on " + tojson(secondary));
    assert.commandWorked(secondary.getDB("admin").adminCommand({
        configureFailPoint: "failCollectionInserts",
        mode: {times: 2},
        data: {collectionNS: coll.getFullName()}
    }));

    jsTest.log("Issuing RESYNC command to " + tojson(secondary));
    assert.commandWorked(secondary.getDB("admin").runCommand({resync: 1}));

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    assert.eq(1, secondary.getDB("test")[name].count());
    assert.docEq({_id: 0, x: 1}, secondary.getDB("test")[name].findOne());

    jsTest.log("Stopping repl set test; finished.");
    replSet.stopSet();
})();
