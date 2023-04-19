/**
 * This tests that errors are logged when dbCheck finds evidence of corruption, but does not cause
 * the operation to fail.
 */
(function() {

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();

const db = primary.getDB('test');
const collName = 'coll';
const coll = db[collName];

assert.commandWorked(coll.insert({_id: 0, a: "first"}));

// Create the same type of corruption on both nodes.
assert.commandWorked(db.adminCommand({
    configureFailPoint: "skipUnindexingDocumentWhenDeleted",
    mode: "alwaysOn",
    data: {indexName: "_id_"}
}));
assert.commandWorked(secondary.getDB('admin').runCommand({
    configureFailPoint: "skipUnindexingDocumentWhenDeleted",
    mode: "alwaysOn",
    data: {indexName: "_id_"}
}));

const docId = 1;
assert.commandWorked(coll.insert({_id: docId, a: "second"}));
assert.commandWorked(coll.remove({_id: docId}));

// Validate should detect this inconsistency.
let res = coll.validate();
assert.commandWorked(res);
assert(!res.valid, res);

assert.commandWorked(db.runCommand({"dbCheck": 1}));

// Wait for both nodes to finish checking.
const healthlogSecondary = secondary.getDB('local').system.healthlog;
assert.soon(() => healthlogSecondary.find({operation: "dbCheckStop"}).itcount() == 1);

[primary, secondary].forEach((node) => {
    print("checking " + tojson(node));
    let entry = node.getDB('local').system.healthlog.findOne({severity: 'error'});
    assert(entry, "No healthlog entry found on " + tojson(node));
    assert.eq("Erroneous index key found with reference to non-existent record id",
              entry.msg,
              tojson(entry));

    // The erroneous index key should not affect the hashes. The documents should still be the same.
    assert.eq(1, node.getDB('local').system.healthlog.count({severity: 'error'}));
});

replSet.stopSet(undefined /* signal */, false /* forRestart */, {skipValidation: true});
})();
