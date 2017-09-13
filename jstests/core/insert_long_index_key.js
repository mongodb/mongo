t = db.insert_long_index_key;
t.drop();

var s = new Array(2000).toString();
t.ensureIndex({x: 1});

t.insert({x: 1});
t.insert({x: s});

assert.eq(1, t.count());
