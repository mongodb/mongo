/*
 * Test a nested $or query which reproduces SERVER-84013, a bug in the subplanner. This bug had to
 * do with the subplanner assuming that multiple invocations of MatchExpression::optimize() yielded
 * the same expressions, which turns out not to be the case. The queries in this regression test
 * excerise the $or -> $in rewrite which produce new $in expressions which themselves could be
 * further optimized.
 */
(function() {
"use strict";

const coll = db.server84013;
coll.drop();

const docs = [
    {_id: 0, Country: {_id: "US"}, State: "California", City: "SanFrancisco"},
    {_id: 1, Country: {_id: "US"}, State: "NewYork", City: "Buffalo"},
];

assert.commandWorked(coll.insert(docs));
assert.commandWorked(coll.createIndex({"Country._id": 1, "State": 1}));

assert.eq(docs.slice(0, 1),
          coll.find({
                  "$or": [
                      {"Country._id": "DNE"},
                      {
                          "Country._id": "US",
                          "State": "California",
                          "$or": [{"City": "SanFrancisco"}, {"City": {"$in": ["SanFrancisco"]}}]
                      }
                  ]
              })
              .toArray());

assert.eq(docs.slice(0, 1),
          coll.find({
                  "$or": [
                      {"Country._id": "DNE"},
                      {
                          "Country._id": "US",
                          "State": "California",
                          "$or": [
                              {"City": "SanFrancisco"},
                              {"City": {$in: ["SanFrancisco"]}},
                              {"Country._id": "DNE"},
                          ]
                      },
                  ]
              })
              .toArray());
})();
