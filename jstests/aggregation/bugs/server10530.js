// SERVER-10530 Would error if large objects are in first batch

var t = db.server10530;
t.drop();

t.insert({big: Array(1024*1024).toString()});
t.insert({big: Array(16*1024*1024 - 1024).toString()});
t.insert({big: Array(1024*1024).toString()});

assert.eq(t.aggregateCursor().itcount(), 3);

// clean up large collection
t.drop();
