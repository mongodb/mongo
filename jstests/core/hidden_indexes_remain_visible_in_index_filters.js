/**
 * Test that the hidden indexes will still be visible for use by index filters.
 *
 * @tags: [
 *      # Command 'planCacheSetFilter' may return different values after a failover.
 *      does_not_support_stepdowns,
 * ]
 */

(function() {
"use strict";

const collName = 'hidden_indexes_remain_visible_in_index_filters';
db[collName].drop();
const coll = db[collName];

assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));
assert.commandWorked(coll.createIndex({a: 1}));

const queryShape = {
    query: {a: 1},
    sort: {a: -1},
    projection: {_id: 0, a: 1}
};

// Ensure the filters for the given query shape exsit.
function ensureFilterExistsByQueryShape(queryShape) {
    const res = assert.commandWorked(coll.runCommand('planCacheListFilters'));
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    const filter = res.filters.find(function(obj) {
        return tojson(obj.query) === tojson(queryShape.query) &&
            tojson(obj.projection) === tojson(queryShape.projection) &&
            tojson(obj.sort) === tojson(queryShape.sort);
    });

    assert(filter, `Index filter not found for query shape ${tojson(queryShape)}`);
}

// Add index filters for simple query.
assert.commandWorked(coll.runCommand('planCacheSetFilter', {
    query: queryShape.query,
    sort: queryShape.sort,
    projection: queryShape.projection,
    indexes: [{a: 1}]
}));
ensureFilterExistsByQueryShape(queryShape);

// Hide the index. Hiding the index will not have impact on index filters.
assert.commandWorked(coll.hideIndex("a_1"));
ensureFilterExistsByQueryShape(queryShape);
})();
