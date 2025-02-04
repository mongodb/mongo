const t = db[jsTestName()];

t.drop();
let x =
    t.runCommand("findAndModify", {query: {f: 1}, update: {$set: {f: 2}}, upsert: true, new: true});
let le = x.lastErrorObject;
assert.eq(le.updatedExisting, false);
assert.eq(le.n, 1);
assert.eq(le.upserted, x.value._id);

assert(t.drop());
assert.commandWorked(t.insert({f: 1}));
x = t.runCommand("findAndModify", {query: {f: 1}, remove: true});
le = x.lastErrorObject;
assert.eq(le.n, 1);
