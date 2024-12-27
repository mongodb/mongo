/**
 * Test OR-pushdown fixes for elemMatch based on SERVER-74954.
 *  @tags: [
 *    requires_getmore,
 *  ]
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.jstests_elemmatch_or_pushdown_paths;

coll.drop();

assert.commandWorked(coll.insert([
    {a: 1, b: [{c: 1}]},
    {a: 2, b: [{c: 1}]},
    {a: 3, b: [{c: 1}]},
    {a: 4, b: [{c: 1}]},
]));
assert.commandWorked(coll.createIndex({"b.c": 1, a: 1}));

// Test exact bounds.
assert(arrayEq(coll.find({
                       $and: [
                           {$or: [{a: {$lt: 2}}, {a: {$gt: 3}}]},
                           {b: {$elemMatch: {c: {$eq: 1, $exists: true}}}}
                       ]
                   },
                         {_id: 0})
                   .hint({"b.c": 1, a: 1})
                   .toArray(),
               [
                   {a: 1, b: [{c: 1}]},
                   {a: 4, b: [{c: 1}]},
               ]));

// Similar test, but use $mod instead of $exists.
const results = coll.find({
                        $and: [
                            {$or: [{a: {$lt: 2}}, {a: {$gt: 3}}]},
                            {b: {$elemMatch: {c: {$eq: 1, $mod: [2, 1]}}}}
                        ]
                    },
                          {_id: 0})
                    .toArray();

assert(arrayEq(results,
               [
                   {a: 1, b: [{c: 1}]},
                   {a: 4, b: [{c: 1}]},
               ]),
       results);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {a: 5, b: [{c: 5, d: 6, e: 7}]},
    {a: 5, b: [{c: 5, d: 6, e: 8}]},
    {a: 5, b: [{c: 5, d: 5, e: 7}]},
    {a: 4, b: [{c: 5, d: 6, e: 7}]},
]));
assert.commandWorked(coll.createIndex({"b.d": 1, "b.c": 1}));
assert.commandWorked(coll.createIndex({"b.e": 1, "b.c": 1}));

// Test OR within elemmatch.
assert(arrayEq(
    coll.find({$and: [{a: 5}, {b: {$elemMatch: {$and: [{c: 5}, {$or: [{d: 6}, {e: 7}]}]}}}]},
              {_id: 0})
        .toArray(),
    [
        {a: 5, b: [{c: 5, d: 6, e: 7}]},
        {a: 5, b: [{c: 5, d: 6, e: 8}]},
        {a: 5, b: [{c: 5, d: 5, e: 7}]},
    ]));