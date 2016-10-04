// Test basic delete stage functionality.
var coll = db.stages_delete;
var collScanStage = {cscan: {args: {direction: 1}, filter: {deleteMe: true}}};
var deleteStage;

// Test delete stage with isMulti: true.
coll.drop();
assert.writeOK(coll.insert({deleteMe: true}));
assert.writeOK(coll.insert({deleteMe: true}));
assert.writeOK(coll.insert({deleteMe: false}));
deleteStage = {
    delete: {args: {node: collScanStage, isMulti: true}}
};
assert.eq(coll.count(), 3);
assert.commandWorked(db.runCommand({stageDebug: {collection: coll.getName(), plan: deleteStage}}));
assert.eq(coll.count(), 1);
assert.eq(coll.count({deleteMe: false}), 1);

// Test delete stage with isMulti: false.
coll.drop();
assert.writeOK(coll.insert({deleteMe: true}));
assert.writeOK(coll.insert({deleteMe: true}));
assert.writeOK(coll.insert({deleteMe: false}));
deleteStage = {
    delete: {args: {node: collScanStage, isMulti: false}}
};
assert.eq(coll.count(), 3);
assert.commandWorked(db.runCommand({stageDebug: {collection: coll.getName(), plan: deleteStage}}));
assert.eq(coll.count(), 2);
assert.eq(coll.count({deleteMe: true}), 1);
assert.eq(coll.count({deleteMe: false}), 1);
