const t = db.test_validate_passes;
t.drop();

assert.commandWorked(t.insert({_id: 1, x: 1}));
