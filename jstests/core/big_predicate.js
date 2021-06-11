/**
 * Test that a query containing an AND with a lot of clauses can be answered.
 *
 * The explain for this can take long enough to make tests with stepdowns fail.
 * @tags: [operations_longer_than_stepdown_interval]
 */
(function() {
"use strict";

const coll = db.big_predicate;
coll.drop();

let filter = {};
for (let i = 0; i < 2500; ++i) {
    filter["field" + i] = 123;
}

assert.commandWorked(coll.insert({foo: 1}));
assert.commandWorked(coll.insert(filter));

assert.eq(coll.find(filter).itcount(), 1);
assert.commandWorked(coll.explain().find(filter).finish());
})();
