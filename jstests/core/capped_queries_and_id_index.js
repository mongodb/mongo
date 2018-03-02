// Tests the behavior of querying or updating a capped collection with and without an _id index.
(function() {
    "use strict";
    const coll = db.capped9;
    coll.drop();

    assert.commandWorked(db.createCollection("capped9", {capped: true, size: 1024 * 50}));

    assert.writeOK(coll.insert({_id: 1, x: 2, y: 3}));

    assert.eq(1, coll.find({x: 2}).itcount());
    assert.eq(1, coll.find({y: 3}).itcount());

    // SERVER-3064 proposes making the following queries/updates by _id result in an error.
    assert.eq(1, coll.find({_id: 1}).itcount());
    assert.writeOK(coll.update({_id: 1}, {$set: {y: 4}}));
    assert.eq(4, coll.findOne().y);

    assert.commandWorked(coll.createIndex({_id: 1}));
    assert.eq(1, coll.find({_id: 1}).itcount());
    assert.writeOK(coll.update({_id: 1}, {$set: {y: 5}}));
    assert.eq(5, coll.findOne().y);
}());
