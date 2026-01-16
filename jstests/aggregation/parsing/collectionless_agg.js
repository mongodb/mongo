/**
 * @tags: [
 *   # Do not run with facets as $documents is not allowed to be used within a $facet stage.
 *   do_not_wrap_aggregations_in_facets,
 *   requires_fcv_83
 *
 * ]
 */

import {assertErrMsgContains} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const errMsgNew = "can only be run with database or cluster-level aggregation";

// Check the expected error message is thrown.
let pipeline = [{$documents: [{nb: [1, 2, 3]}]}, {$project: {total: {$sum: ["$nb"]}}}];
assertErrMsgContains(coll, pipeline, errMsgNew);

// Running aggregation on the database level should not produce errors.
assert.commandWorked(db.runCommand({aggregate: 1, pipeline: pipeline, cursor: {}}));
