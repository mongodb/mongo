// test listIndexes bad usage

t = db.list_indexes2;
t.drop();
t.insert({value: "dummy"});

// non-string in argument
err = db.runCommand({listIndexes: 1});
assert.commandFailed(err);
assert.eq(err.code, 28528);

// empty string in argument
err = db.runCommand({listIndexes: ''});
assert.commandFailed(err);
assert.eq(err.code, 28529);

// non-existing collection in argument
db.list_listindexes2_no_such_collection.drop();
err = db.runCommand({listIndexes: 'list_listindexes2_no_such_collection'});
assert.commandFailed(err);
assert.eq(err.code, 26);

