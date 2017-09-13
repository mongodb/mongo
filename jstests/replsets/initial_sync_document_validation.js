/**
 * Tests that initial sync via resync command does not fail if it inserts documents
 * which don't validate.
 */

(function() {
    var name = 'initial_sync_document_validation';
    var replSet = new ReplSetTest({
        name: name,
        nodes: 2,
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();
    var secondary = replSet.getSecondary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, x: 1}));
    assert.commandWorked(coll.runCommand("collMod", {"validator": {a: {$exists: true}}}));

    assert.commandWorked(secondary.getDB("admin").runCommand({resync: 1}));
    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    assert.eq(1, secondary.getDB("test")[name].count());
    assert.docEq({_id: 0, x: 1}, secondary.getDB("test")[name].findOne());

    replSet.stopSet();
})();
