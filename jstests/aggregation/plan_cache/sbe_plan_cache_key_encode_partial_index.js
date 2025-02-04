/**
 * SERVER-94212: The SBE plan cache encoding does not properly encode the discriminator for a
 * partial index when the subplanner is invoked. This results in a plan cache entry being reused for
 * a query which is not eligible to use the partial index and results in missing documents.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, "partial_index_subplanner_cache");

// Create a partial index on date.
assert.commandWorked(coll.createIndex(
    {'date': 1}, {partialFilterExpression: {'date': {$gt: new Date('2019-05-22T21:56:29.813Z')}}}));
assert.commandWorked(coll.createIndex({'str': 1}));

// Insert a document which is not covered by the partial index.
assert.commandWorked(coll.insert({
    _id: 212,
    'date': new Date('2019-03-20T20:07:30.099Z'),
}));

// Run an aggregation with a $match stage that requires the subplanner (rooted $or) and a SBE
// eligible $group. The predicate in the $match on date is eligible to use the partial index on the
// date field. We don't actually care about the result, we do this in order to put an entry into the
// plan cache, specifically, the entry will have a plan that uses the date index.
coll.aggregate([
        {
            $match: {
                $or: [
                    {'date': {$gte: new Date('2019-11-27T01:24:04.988Z')}},
                    {'str': 'abc'},
                ]
            }
        },
        {$group: {_id: null, m: {$min: '$date'}}}
    ])
    .toArray();

// Run an aggregation with the same query shape as above. The difference is that the predicate on
// date is no longer eligible to use the partial index on date. But due to this bug, this query ends
// up hashing to the same key as above and using the cached plan. This leads to incorrect results
// because the index scan (on the partial index) doesn't contain the document we're looking for.
const resWithSbe = coll.aggregate([
                           {
                               $match: {
                                   $or: [
                                       {'date': {$gte: new Date('2019-01-13T21:32:59.727Z')}},
                                       {'str': 'abc'},
                                   ]
                               }
                           },
                           {$group: {_id: null, m: {$min: '$date'}}}
                       ])
                       .toArray();

assert.eq(1, resWithSbe.length);
