// Tests for predicates which can use the trailing field of a 2d index.
(function() {
    "use strict";

    const coll = db.geo_2d_trailing_fields;

    const isMaster = assert.commandWorked(db.adminCommand({isMaster: 1}));
    const isMongos = (isMaster.msg === "isdbgrid");

    coll.drop();
    assert.commandWorked(coll.createIndex({a: "2d", b: 1}));
    assert.writeOK(coll.insert({a: [0, 0]}));

    // Verify that $near queries handle existence predicates over the trailing fields correctly.
    if (!isMongos) {
        assert.eq(0, coll.find({a: {$near: [0, 0]}, b: {$exists: true}}).itcount());
        assert.eq(1, coll.find({a: {$near: [0, 0]}, b: null}).itcount());
        assert.eq(1, coll.find({a: {$near: [0, 0]}, b: {$exists: false}}).itcount());
    }

    // Verify that non-near 2d queries handle existence predicates over the trailing fields
    // correctly.
    assert.eq(0,
              coll.find({a: {$geoWithin: {$center: [[0, 0], 1]}}, b: {$exists: true}}).itcount());
    assert.eq(1, coll.find({a: {$geoWithin: {$center: [[0, 0], 1]}}, b: null}).itcount());
    assert.eq(1,
              coll.find({a: {$geoWithin: {$center: [[0, 0], 1]}}, b: {$exists: false}}).itcount());

    coll.drop();
    assert.commandWorked(coll.createIndex({a: "2d", "b.c": 1}));
    assert.writeOK(coll.insert({a: [0, 0], b: [{c: 2}, {c: 3}]}));

    // Verify that $near queries correctly handle predicates which cannot be covered due to array
    // semantics.
    if (!isMongos) {
        assert.eq(0, coll.find({a: {$near: [0, 0]}, "b.c": [2, 3]}).itcount());
        assert.eq(0, coll.find({a: {$near: [0, 0]}, "b.c": {$type: "array"}}).itcount());
    }

    // Verify that non-near 2d queries correctly handle predicates which cannot be covered due to
    // array semantics.
    assert.eq(0, coll.find({a: {$geoWithin: {$center: [[0, 0], 1]}}, "b.c": [2, 3]}).itcount());
    assert.eq(
        0, coll.find({a: {$geoWithin: {$center: [[0, 0], 1]}}, "b.c": {$type: "array"}}).itcount());
}());
