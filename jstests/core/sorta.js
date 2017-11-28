// SERVER-2905 sorting with missing fields

(function() {
    'use strict';

    var coll = db.jstests_sorta;
    coll.drop();

    const docs = [
        {_id: 0, a: MinKey},
        {_id: 1, a: []},
        {_id: 2, a: []},
        {_id: 3, a: null},
        {_id: 4},
        {_id: 5, a: null},
        {_id: 6, a: 1},
        {_id: 7, a: [2]},
        {_id: 8, a: MaxKey}
    ];
    const bulk = coll.initializeUnorderedBulkOp();
    for (let doc of docs) {
        bulk.insert(doc);
    }
    assert.writeOK(bulk.execute());

    assert.eq(coll.find().sort({a: 1, _id: 1}).toArray(), docs);

    assert.commandWorked(coll.createIndex({a: 1, _id: 1}));
    assert.eq(coll.find().sort({a: 1, _id: 1}).toArray(), docs);
})();
