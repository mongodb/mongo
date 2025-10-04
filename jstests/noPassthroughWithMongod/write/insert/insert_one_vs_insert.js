// Test insert and insertOne with write-concerns other than {w:1}.

const coll = db.testInserts;
coll.drop();

const thrown = assert.throws(() => coll.insertOne({a: 1}, {w: 2}));
assert(thrown instanceof WriteCommandError);

const insertResult = coll.insert({a: 2}, {w: 2});
assert(insertResult instanceof WriteCommandError);
assert.eq(insertResult.message, "cannot use 'w' > 1 when a host is not replicated");

assert.eq(coll.count(), 0);
