// Test that client cannot access index namespaces SERVER-4276.

t = db.jstests_indexi;
t.drop();

idx = db.jstests_indexi.$_id_;

var expectWriteError = function(func) {
    if (db.getMongo().writeMode() == 'commands') {
        assert.throws(func);
    }
    else {
        assert.writeError(func());
    }
};

// Test that accessing the index namespace fails.
function checkFailingOperations() {
    assert.throws(function() { idx.find().itcount(); });
    expectWriteError(function() { return idx.insert({ x: 1 }); });
    expectWriteError(function() { return idx.update({ x: 1 }, { x: 2 }); });
    expectWriteError(function() { return idx.remove({ x: 1 }); });
    assert.commandFailed( idx.runCommand( 'compact' ) );
    assert.writeError(idx.ensureIndex({ x: 1 }));
}

// Check with base collection not present.
// TODO: SERVER-4276
//checkFailingOperations();
t.save({});

// Check with base collection present.
checkFailingOperations();

