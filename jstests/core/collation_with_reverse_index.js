// Regression test for SERVER-34846.
(function() {
    const coll = db.collation_with_reverse_index;
    coll.drop();

    coll.insertOne({int: 1, text: "hello world"});
    coll.createIndex({int: -1, text: -1}, {collation: {locale: "en", strength: 1}});
    const res = coll.find({int: 1}, {_id: 0, int: 1, text: 1}).toArray();

    assert.eq(res.length, 1);
    assert.eq(res[0].text, "hello world");
})();
