// Test basic behavior for no-op writes.
(function() {
"use strict";

// Use a high enough number of documents so background operations in passthrough tests, e.g.
// failovers or migrations, can reliably occur.
const kNumDocs = 20;

const coll = db.noop_writes;
coll.drop();

function deleteAllDocs(coll) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumDocs; i++) {
        // Use single deletes so this test is compatible with failover suites.
        bulk.find({_id: i}).removeOne();
    }
    assert.commandWorked(bulk.execute());
}

//
// No-op update.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(coll.update({_id: i}, {$set: {x: i}}));

    const res = assert.commandWorked(coll.update({_id: i}, {$set: {x: i}}));
    assert.eq(res.nMatched, 1, tojson(res));
    if (coll.getMongo().writeMode() == "commands") {
        assert.eq(res.nModified, 0, tojson(res));
        assert.eq(res.nUpserted, 0, tojson(res));
    }

    assert.eq({_id: i, x: i}, coll.findOne({_id: i, x: i}), "No matching document for i = " + i);
}

// Reset the collection for the next test case.
deleteAllDocs(coll);

//
// No-op update, no matching document.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(coll.remove({_id: i}, true /* justOne */));

    const res = assert.commandWorked(coll.update({_id: i}, {$set: {x: i}}));
    assert.eq(res.nMatched, 0, tojson(res));
    if (coll.getMongo().writeMode() == "commands") {
        assert.eq(res.nModified, 0, tojson(res));
        assert.eq(res.nUpserted, 0, tojson(res));
    }

    assert.isnull(coll.findOne({_id: i}), "Unexpected matching document for i = " + i);
}

//
// No-op findAndModify update.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.eq({_id: i}, coll.findAndModify({query: {_id: i}, update: {$set: {x: i}}}));

    assert.eq({_id: i, x: i}, coll.findAndModify({query: {_id: i}, update: {$set: {x: i}}}));

    assert.eq({_id: i, x: i}, coll.findOne({_id: i, x: i}), "No matching document for i = " + i);
}

// Reset the collection for the next test case.
deleteAllDocs(coll);

//
// No-op findAndModify update, no matching document.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(coll.remove({_id: i}, true /* justOne */));

    assert.isnull(coll.findAndModify({query: {_id: i}, update: {$set: {x: i}}}));

    assert.isnull(coll.findOne({_id: i}), "Unexpected matching document for i = " + i);
}

//
// No-op delete.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(coll.remove({_id: i}, true /* justOne */));

    const res = assert.commandWorked(coll.remove({_id: i}, true /* justOne */));
    assert.eq(res.nRemoved, 0, tojson(res));

    assert.isnull(coll.findOne({_id: i}), "Unexpected matching document for i = " + i);
}

// Reset the collection for the next test case.
deleteAllDocs(coll);

//
// No-op findAndModify delete.
//

for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.eq({_id: i}, coll.findAndModify({query: {_id: i}, remove: true}));

    assert.isnull(coll.findAndModify({query: {_id: i}, remove: true}));

    assert.isnull(coll.findOne({_id: i}), "Unexpected matching document for i = " + i);
}
})();
