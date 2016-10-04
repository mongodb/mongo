// check the insertMulti path works, including the error handling

(function() {
    "use strict";

    function makeDocument(docSize) {
        var doc = {"fieldName": ""};
        var longString = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        while (Object.bsonsize(doc) < docSize) {
            if (Object.bsonsize(doc) < docSize - longString.length) {
                doc.fieldName += longString;
            } else {
                doc.fieldName += "x";
            }
        }
        return doc;
    }

    db.getMongo().forceWriteMode('legacy');
    var t = db.foo;

    t.drop();
    t.insert([{_id: 1}, {_id: 2}]);
    assert.eq(t.count(), 2);
    t.insert([{_id: 3}, {_id: 2}, {_id: 4}], 0);  // no ContinueOnError
    assert.eq(t.count(), 3);
    assert.eq(t.count({"_id": 1}), 1);
    assert.eq(t.count({"_id": 2}), 1);
    assert.eq(t.count({"_id": 3}), 1);
    assert.eq(t.count({"_id": 4}), 0);

    t.drop();
    t.insert([{_id: 1}, {_id: 2}]);
    assert.eq(t.count(), 2);
    t.insert([{_id: 3}, {_id: 2}, {_id: 4}], 1);  // ContinueOnError
    assert.eq(t.count(), 4);
    assert.eq(t.count({"_id": 1}), 1);
    assert.eq(t.count({"_id": 2}), 1);
    assert.eq(t.count({"_id": 3}), 1);
    assert.eq(t.count({"_id": 4}), 1);

    // Push a large vector in bigger than the subset size we'll break it up into
    t.drop();
    var doc = makeDocument(16 * 1024);
    var docs = [];
    for (var i = 0; i < 1500; i++)
        docs.push(Object.extend({}, doc));
    t.insert(docs);
    assert.eq(t.count(), docs.length);
})();
