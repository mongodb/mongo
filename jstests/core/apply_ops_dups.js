(function() {
    "use strict";
    var t = db.apply_ops_dups;
    t.drop();

    // Check that duplicate _id fields don't cause an error
    assert.writeOK(t.insert({_id: 0, x: 1}));
    assert.commandWorked(t.createIndex({x: 1}, {unique: true}));
    var a = assert.commandWorked(db.adminCommand({
        applyOps: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: -1}},
            {"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 0}}
        ]
    }));
    printjson(a);
    printjson(t.find().toArray());
    assert.eq(2, t.find().count(), "Invalid insert worked");
    assert.eq(true, a.results[0], "Valid insert was rejected");
    assert.eq(true, a.results[1], "Insert should have not failed (but should be ignored");
    printjson(t.find().toArray());

    // Check that dups on non-id cause errors
    var a = assert.commandFailedWithCode(db.adminCommand({
        applyOps: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 1, x: 0}},
            {"op": "i", "ns": t.getFullName(), "o": {_id: 2, x: 1}}
        ]
    }),
                                         11000 /*DuplicateKey*/);
    assert.eq(2, t.find().count(), "Invalid insert worked");
})();
