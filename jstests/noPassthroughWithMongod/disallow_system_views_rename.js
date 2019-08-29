(function() {
'use strict';
assert.commandWorked(db.runCommand({insert: 'a', documents: [{x: 1}]}));

// Disallow renaming to system.views
assert.commandFailedWithCode(db.adminCommand({renameCollection: 'test.a', to: 'test.system.views'}),
                             ErrorCodes.IllegalOperation);

assert.commandWorked(db.createView('viewA', 'a', []));

// Disallow renaming from system.views
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: 'test.system.views', to: 'test.aaabb'}),
    ErrorCodes.IllegalOperation);

// User can still rename system.views (or to system.views) via applyOps command.
assert.commandWorked(db.adminCommand({
    applyOps: [{op: 'c', ns: 'test.$cmd', o: {renameCollection: 'test.system.views', to: 'test.b'}}]
}));

assert.commandWorked(db.adminCommand({
    applyOps: [{op: 'c', ns: 'test.$cmd', o: {renameCollection: 'test.b', to: 'test.system.views'}}]
}));
})();
