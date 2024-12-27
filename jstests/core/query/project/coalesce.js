/**
 * Tests projections that can be coalesced.
 * @tags: [
 *   requires_getmore,
 * ]
 */

import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.projection_coalescence;
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}));

assert(resultsEq(coll.aggregate([{$project: {a: 0, b: 0}}]).toArray(),
                 coll.aggregate([{$project: {a: 0}}, {$project: {b: 0}}]).toArray()));

assert.commandWorked(coll.insert({a: {a2: 1}, b: 1, c: 1}));

assert(resultsEq(coll.aggregate([{$project: {"a.a2": 0, b: 0}}]).toArray(),
                 coll.aggregate([{$project: {"a.a2": 0}}, {$project: {b: 0}}]).toArray()));

assert(resultsEq(coll.aggregate([{$project: {"a.a2": 0, b: 0}}]).toArray(),
                 coll.aggregate([{$project: {b: 0}}, {$project: {"a.a2": 0}}]).toArray()));
