// Verify that $expr can be used in the top-level position, but not in subdocuments.

(function() {
    "use strict";

    const coll = db.expr_valid_positions;

    // Works at the BSON root level.
    assert.eq(0, coll.find({$expr: {$eq: ["$foo", "$bar"]}}).itcount());

    // Works inside a $or.
    assert.eq(0, coll.find({$or: [{$expr: {$eq: ["$foo", "$bar"]}}, {b: {$gt: 3}}]}).itcount());

    // Fails inside an elemMatch.
    assert.throws(function() {
        coll.find({a: {$elemMatch: {$expr: {$eq: ["$foo", "$bar"]}}}}).itcount();
    });

    // Fails inside an _internalSchemaObjectMatch.
    assert.throws(function() {
        coll.find({a: {$_internalSchemaObjectMatch: {$expr: {$eq: ["$foo", "$bar"]}}}}).itcount();
    });
}());