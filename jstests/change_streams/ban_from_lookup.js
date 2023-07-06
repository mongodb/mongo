/**
 * Test that the $changeStream stage cannot be used in a $lookup pipeline or sub-pipeline.
 *
 * @tags: [
 *   change_stream_does_not_expect_txns,
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, "change_stream_ban_from_lookup");
const foreignColl = "unsharded";

assert.commandWorked(coll.insert({_id: 1}));

// Verify that we cannot create a $lookup using a pipeline which begins with $changeStream.
assertErrorCode(
    coll, [{$lookup: {from: foreignColl, as: 'as', pipeline: [{$changeStream: {}}]}}], 51047);

// Verify that we cannot create a $lookup if its pipeline contains a sub-$lookup whose pipeline
// begins with $changeStream.
assertErrorCode(
        coll,
        [{
           $lookup: {
               from: foreignColl,
               as: 'as',
               pipeline: [
                   {$match: {_id: 1}},
                   {$lookup: {from: foreignColl, as: 'subas', pipeline: [{$changeStream: {}}]}}
               ]
           }
        }],
        51047);
