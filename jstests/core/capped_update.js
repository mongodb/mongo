/**
 * Tests various update scenarios on capped collections:
 *  -- SERVER-20529: Ensure capped document sizes do not change
 *  -- SERVER-11983: Don't create _id field on capped updates
 */
(function() {
    'use strict';
    var t = db.cannot_change_capped_size;
    t.drop();
    assert.commandWorked(
        db.createCollection(t.getName(), {capped: true, size: 1024, autoIndexId: false}));
    assert.eq(0, t.getIndexes().length, "the capped collection has indexes");

    for (var j = 1; j <= 10; j++) {
        assert.writeOK(t.insert({_id: j, s: "Hello, World!"}));
    }

    assert.writeOK(t.update({_id: 3}, {s: "Hello, Mongo!"}));  // Mongo is same length as World
    assert.writeError(t.update({_id: 3}, {$set: {s: "Hello!"}}));
    assert.writeError(t.update({_id: 10}, {}));
    assert.writeError(t.update({_id: 10}, {s: "Hello, World!!!"}));

    assert.commandWorked(t.getDB().runCommand({godinsert: t.getName(), obj: {a: 2}}));
    var doc = t.findOne({a: 2});
    assert.eq(undefined, doc["_id"], "now has _id after godinsert");
    assert.writeOK(t.update({a: 2}, {$inc: {a: 1}}));
    doc = t.findOne({a: 3});
    assert.eq(undefined, doc["_id"], "now has _id after update");
})();
