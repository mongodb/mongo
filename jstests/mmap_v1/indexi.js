// Test that client cannot access index namespaces.

t = db.jstests_indexi;
t.drop();

idx = db.jstests_indexi.$_id_;

// Test that accessing the index namespace fails.
function checkFailingOperations() {
    assert.writeError(idx.insert({x: 1}));
    assert.writeError(idx.update({x: 1}, {x: 2}));
    assert.writeError(idx.remove({x: 1}));
    assert.commandFailed(idx.runCommand('compact'));
    assert.commandFailed(idx.ensureIndex({x: 1}));
}

// Check with base collection not present.
checkFailingOperations();
t.save({});

// Check with base collection present.
checkFailingOperations();
