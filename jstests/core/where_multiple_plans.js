/**
 * Tests that a $where predicate works as expected when there are multiple candidate plans.
 *
 * @tags: [
 *   requires_fcv_60,
 *   requires_scripting,
 * ]
 */
(function() {
"use strict";

const coll = db.where_multiple_plans;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(coll.insert({_id: 0, a: 1, b: 3, c: 4}));
assert.commandWorked(coll.insert({_id: 1, a: 2, b: 1, c: 4}));
assert.commandWorked(coll.insert({_id: 2, a: 2, b: 3, c: 1}));
assert.commandWorked(coll.insert({_id: 3, a: 2, b: 3, c: 4}));
assert.commandWorked(coll.insert({_id: 4, a: 2, b: 3, c: 4}));

assert.eq(2,
          coll.find({
                  a: {$eq: 2},
                  b: {$eq: 3},
                  $where: function() {
                      return this.c == 4;
                  }
              })
              .itcount());
}());
