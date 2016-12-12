// Ensure an index cannot be created on system.indexes
(function() {
    var t = db.getSiblingDB("indexes_on_indexes");
    t.dropDatabase();
    t.coll.insert({});

    printjson(t.system.indexes.getIndexes());
    assert.eq(t.system.indexes.getIndexes().length, 0);

    print("trying via ensureIndex");
    assert.commandFailed(t.system.indexes.ensureIndex({_id: 1}));
    printjson(t.system.indexes.getIndexes());
    assert.eq(t.system.indexes.getIndexes().length, 0);

    print("trying via createIndex");
    assert.commandFailed(t.system.indexes.createIndex({_id: 1}));
    printjson(t.system.indexes.getIndexes());
    assert.eq(t.system.indexes.getIndexes().length, 0);

    print("trying via direct insertion");
    assert.writeErrorWithCode(
        t.system.indexes.insert(
            {v: 1, key: {_id: 1}, ns: "indexes_on_indexes.system.indexes", name: "wontwork"}),
        ErrorCodes.CannotCreateIndex);
    printjson(t.system.indexes.getIndexes());
    assert.eq(t.system.indexes.getIndexes().length, 0);
}());
