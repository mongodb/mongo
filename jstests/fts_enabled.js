// Test that the textSearchEnabled server parameter works correctly (now deprecated).

// Value true is accepted, value false is rejected.
assert.commandWorked(db.adminCommand({setParameter: 1, textSearchEnabled: true}));
assert.commandFailed(db.adminCommand({setParameter: 1, textSearchEnabled: false}));
