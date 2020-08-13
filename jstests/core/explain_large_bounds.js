/**
 * Make sure explain succeeds even when the index bounds are really big.
 * @tags: [
 *   resource_intensive,
 *   sbe_incompatible,
 * ]
 */
(function() {
const coll = db.jstests_explain_large_bounds;
coll.drop();

coll.ensureIndex({a: 1});

let inClause = [];
for (let i = 0; i < 1000000; i++) {
    inClause.push(i);
}

assert.commandWorked(coll.find({a: {$in: inClause}}).explain());
}());
