// Create a new connection object so it won't affect the global connection when we modify
// it's settings.
var conn = new Bongo(db.getBongo().host);
conn._skipValidation = true;
conn.forceWriteMode(db.getBongo().writeMode());

t = conn.getDB(db.getName()).insert2;
t.drop();

assert.isnull(t.findOne(), "A");
assert.writeError(t.insert({z: 1, $inc: {x: 1}}, 0, true));
assert.isnull(t.findOne(), "B");
// Collection should not exist
assert.commandFailed(t.stats());
