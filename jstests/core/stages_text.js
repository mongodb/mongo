// Test very basic functionality of text stage

t = db.stages_text;
t.drop();
var collname = "stages_text";

t.save({x: "az b x"});

t.ensureIndex({x: "text"});

// We expect to retrieve 'b'
res = db.runCommand({stageDebug: {collection: collname, plan: {text: {args: {search: "b"}}}}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// I have not been indexed yet.
res = db.runCommand({stageDebug: {collection: collname, plan: {text: {args: {search: "hari"}}}}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 0);
