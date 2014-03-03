// Test very basic functionality of text stage

t = db.stages_text;
t.drop();
t.save({x: "az b x"})

t.ensureIndex({x: "text"})

// We expect to retrieve 'b'
res = db.runCommand({stageDebug: {text: {args: {name: "test.stages_text", search: "b"}}}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// I have not been indexed yet.
res = db.runCommand({stageDebug: {text: {args: {name: "test.stages_text", search: "hari"}}}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 0);
