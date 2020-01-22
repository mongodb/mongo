/*
 * This is a regression test for SERVER-45508, which was an invariant failure in the query planner.
 *
 * Previously the invariant would be triggered only when all of these happen together:
 * - an index with a collation exists
 * - the query planner chooses index bounds with more than one point range
 * - the point ranges are indexed in descending order
 * - more than one index can satisfy the query
 * - the query asks for a sort within each point range
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
    'use strict';

    const coll = db.collation_multi_point_range;
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {collation: {locale: 'fr'}}));
    assert.commandWorked(coll.createIndex({x: -1}));
    assert.commandWorked(coll.createIndex({x: -1, y: 1}));
    coll.insert({x: 2, y: 99});

    assert.commandWorked(coll.find({x: {$in: [2, 5]}}).sort({y: 1}).explain());
    assert.eq(1, coll.find({x: {$in: [2, 5]}}).sort({y: 1}).itcount());
})();
