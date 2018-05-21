(function() {
    "use strict";

    const coll = db.dotted_path_in_null;
    coll.drop();

    assert.writeOK(coll.insert({_id: 1, a: [{b: 5}]}));
    assert.writeOK(coll.insert({_id: 2, a: [{}]}));
    assert.writeOK(coll.insert({_id: 3, a: []}));
    assert.writeOK(coll.insert({_id: 4, a: [{}, {b: 5}]}));
    assert.writeOK(coll.insert({_id: 5, a: [5, {b: 5}]}));

    function getIds(query) {
        let ids = [];
        coll.find(query).sort({_id: 1}).forEach(doc => ids.push(doc._id));
        return ids;
    }

    assert.eq([2, 4], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");

    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.eq([2, 4], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");
}());
