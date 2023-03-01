/**
 * Make sure explain succeeds even when the index bounds are really big.
 * @tags: [
 * resource_intensive,
 * # Exclude the test from every suit in which the balancer is enabled. The migrations
 * # might cause a StaleConfig errors which will be retried for a max number of times before being
 * # reported to the user and cause the test to fail. It has been observed the StaleConfig error to
 * # have high chance of occuring. Most likely given by the slow execution time of the explain
 * # command when the index is large.
 * assumes_balancer_off,
 * ]
 */
(function() {
const coll = db.jstests_explain_large_bounds;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));

let inClause = [];
for (let i = 0; i < 1000000; i++) {
    inClause.push(i);
}

assert.commandWorked(coll.find({a: {$in: inClause}}).explain());
}());
