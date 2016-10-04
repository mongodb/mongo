/**
 * Test that findAndModify works against a non-existent collection.
 */
(function() {
    'use strict';
    var coll = db.find_and_modify_server18054;
    coll.drop();

    assert.eq(null, coll.findAndModify({remove: true}));
    assert.eq(null, coll.findAndModify({update: {$inc: {i: 1}}}));
    var upserted =
        coll.findAndModify({query: {_id: 0}, update: {$inc: {i: 1}}, upsert: true, new: true});
    assert.eq(upserted, {_id: 0, i: 1});

    coll.drop();

    assert.eq(null, coll.findAndModify({remove: true, fields: {z: 1}}));
    assert.eq(null, coll.findAndModify({update: {$inc: {i: 1}}, fields: {z: 1}}));
    assert.eq(null, coll.findAndModify({update: {$inc: {i: 1}}, upsert: true, fields: {z: 1}}));
})();
