/**
 * Tests that only whitelisted stages are permitted to run in a $changeStream pipeline.
 */

(function() {
    "use strict";

    load('jstests/aggregation/extras/utils.js');       // For assertErrorCode.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    const coll = assertDropAndRecreateCollection(db, "change_stream_whitelist");

    // Bare-bones $changeStream pipeline which will be augmented during tests.
    const changeStream = [{$changeStream: {}}];

    // List of non-$changeStream stages which are explicitly whitelisted.
    const whitelist = [
        {$match: {_id: {$exists: true}}},
        {$project: {_id: 1}},
        {$addFields: {newField: 1}},
        {$replaceRoot: {newRoot: {_id: "$_id"}}},
        {$redact: "$$DESCEND"}
    ];

    // List of stages which the whitelist mechanism will prevent from running in a $changeStream.
    // Does not include stages which are blacklisted but already implicitly prohibited, e.g. both
    // $currentOp and $changeStream must be the first stage in a pipeline.
    const blacklist = [
        {$group: {_id: "$_id"}},
        {$sort: {_id: 1}},
        {$skip: 100},
        {$limit: 100},
        {$sample: {size: 100}},
        {$unwind: "$_id"},
        {$lookup: {from: "coll", as: "as", localField: "_id", foreignField: "_id"}},
        {
          $graphLookup: {
              from: "coll",
              as: "as",
              startWith: "$_id",
              connectFromField: "_id",
              connectToField: "_id"
          }
        },
        {$bucketAuto: {groupBy: "$_id", buckets: 2}},
        {$facet: {facetPipe: [{$match: {_id: {$exists: true}}}]}}
    ];

    // Verify that each of the whitelisted stages are permitted to run in a $changeStream.
    for (let allowedStage of whitelist) {
        assert.commandWorked(db.runCommand(
            {aggregate: coll.getName(), pipeline: changeStream.concat(allowedStage), cursor: {}}));
    }

    // Verify that all of the whitelisted stages are able to run in a $changeStream together.
    assert.commandWorked(db.runCommand(
        {aggregate: coll.getName(), pipeline: changeStream.concat(whitelist), cursor: {}}));

    // Verify that a $changeStream pipeline fails to validate if a blacklisted stage is present.
    for (let bannedStage of blacklist) {
        assertErrorCode(coll, changeStream.concat(bannedStage), ErrorCodes.IllegalOperation);
    }
}());