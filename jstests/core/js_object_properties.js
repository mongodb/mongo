// Tests that the properties available on the 'this' object during js execution are only those found
// in the database's BSON object.
// The test runs commands that are not allowed with security token: mapReduce.
// @tags: [
//   not_allowed_with_security_token,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   # Uses mapReduce command.
//   requires_scripting,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For 'resultsEq'.

const testDB = db.getSiblingDB("js_object_properties");
const coll = testDB.test;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1}));

// Test that $where cannot see any other properties.
assert.eq(1,
          coll.find({
                  $where: function() {
                      const properties = new Set(Object.getOwnPropertyNames(this));
                      return properties.has("_id") && properties.has("a") && properties.size == 2;
                  }
              })
              .itcount());

// Test that $function cannot see any other properties.
assert.eq([{field: "_id"}, {field: "a"}],
          coll.aggregate([
                  {
                      $replaceWith: {
                          field: {
                              $function: {
                                  lang: "js",
                                  args: ["$$ROOT"],
                                  body: "function(obj) { return Object.getOwnPropertyNames(obj); }"
                              }
                          }
                      }
                  },
                  {$unwind: "$field"},
                  {$sort: {field: 1}},
              ])
              .toArray());

// Test that the 'this' object doesn't have any properties in $function.
assert.eq([],
          coll.aggregate([
                  {
                      $replaceWith: {
                          field: {
                              $function: {
                                  lang: "js",
                                  args: ["$$ROOT"],
                                  body: "function(obj) { return Object.getOwnPropertyNames(this); }"
                              }
                          }
                      }
                  },
                  {$unwind: "$field"},
                  {$sort: {field: 1}},
              ])
              .toArray());

// Test that mapReduce's 'map' function cannot see any other properties.
assert(resultsEq([{_id: "_id", value: 1}, {_id: "a", value: 1}],
                 assert
                     .commandWorked(coll.mapReduce(
                         function map() {
                             for (let prop of Object.getOwnPropertyNames(this)) {
                                 emit(prop, 1);
                             }
                         },
                         function reduce(key, values) {
                             return Array.sum(values);
                         },
                         {out: {inline: 1}}))
                     .results));

// Test that mapReduce's 'reduce' function cannot see any other properties.
assert.eq([{_id: 0, value: []}],
          assert
              .commandWorked(coll.mapReduce(
                  function map() {
                      emit(this._id, "ignored");
                  },
                  function reduce(key, values) {
                      return Object.getOwnPropertyNames(this);
                  },
                  {out: {inline: 1}}))
              .results);

assert.commandWorked(coll.insert({_id: 1, b: 1}));

// Test that $jsReduce cannot see any properties other than those on the backing db object.
assert.eq([{field: "_id"}, {field: "a"}, {field: "b"}],
          coll.aggregate([
                  {
                      $group: {
                          _id: null,
                          field: {
                              $accumulator: {
                                  init: function() {
                                      return [];
                                  },
                                  accumulate: function acc(state, newObj) {
                                      let stateSet = new Set(state);
                                      for (let prop of Object.getOwnPropertyNames(newObj)) {
                                          stateSet.add(prop);
                                      }
                                      return [...stateSet];
                                  },
                                  accumulateArgs: ["$$ROOT"],
                                  merge: function merge(state1, state2) {
                                      let newState = new Set(state1);
                                      for (let elem of state2) {
                                          newState.add(elem);
                                      }
                                      return [...newState];
                                  },
                                  lang: "js"
                              }
                          }
                      }
                  },
                  {$unset: "_id"},
                  {$unwind: "$field"},
                  {$sort: {field: 1}},
              ])
              .toArray());
}());
