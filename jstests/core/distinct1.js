(function() {
    "use strict";
    const collName = "distinct1";
    const coll = db.getCollection(collName);
    coll.drop();

    assert.eq(0, coll.distinct("a").length, "test empty");

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({a: 3}));

    // Test that distinct returns all the distinct values.
    assert.eq([1, 2, 3], coll.distinct("a").sort(), "distinct returned unexpected results");

    // Test that distinct respects the query condition.
    assert.eq([1, 2],
              coll.distinct("a", {a: {$lt: 3}}).sort(),
              "distinct with query returned unexpected results");

    assert(coll.drop());

    assert.writeOK(coll.insert({a: {b: "a"}, c: 12}));
    assert.writeOK(coll.insert({a: {b: "b"}, c: 12}));
    assert.writeOK(coll.insert({a: {b: "c"}, c: 12}));
    assert.writeOK(coll.insert({a: {b: "c"}, c: 12}));

    // Test that distinct works on fields in embedded documents.
    assert.eq(["a", "b", "c"],
              coll.distinct("a.b").sort(),
              "distinct on dotted field returned unexpected results");

    assert(coll.drop());

    assert.writeOK(coll.insert({_id: 1, a: 1}));
    assert.writeOK(coll.insert({_id: 2, a: 2}));

    // Test that distinct works on the _id field.
    assert.eq([1, 2], coll.distinct("_id").sort(), "distinct on _id returned unexpected results");

    // Test that distinct works with a query on the _id field.
    assert.eq([1],
              coll.distinct("a", {_id: 1}),
              "distinct with query on _id returned unexpected results");

    assert(coll.drop());

    assert.writeOK(coll.insert({a: 1, b: 2}));
    assert.writeOK(coll.insert({a: 2, b: 2}));
    assert.writeOK(coll.insert({a: 2, b: 1}));
    assert.writeOK(coll.insert({a: 2, b: 2}));
    assert.writeOK(coll.insert({a: 3, b: 2}));
    assert.writeOK(coll.insert({a: 4, b: 1}));
    assert.writeOK(coll.insert({a: 4, b: 1}));

    // Test running the distinct command directly, rather than via shell helper.
    let res = assert.commandWorked(db.runCommand({distinct: collName, key: "a"}));
    assert.eq([1, 2, 3, 4], res.values.sort());

    res = assert.commandWorked(db.runCommand({distinct: collName, key: "a", query: null}));
    assert.eq([1, 2, 3, 4], res.values.sort());

    res = assert.commandWorked(db.runCommand({distinct: collName, key: "a", query: {b: 2}}));
    assert.eq([1, 2, 3], res.values.sort());

    assert.commandFailed(db.runCommand({distinct: collName, key: "a", query: 1}));
}());
