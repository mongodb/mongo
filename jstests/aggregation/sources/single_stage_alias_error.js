/**
 * Tests to verify that single aggregation stages that are input into an aggregation pipeline by
 * the user under an aliased name use that name when reporting errors back to the user.
 * @tags: [
 * ]
 */

import {assertErrMsgContains, assertErrMsgDoesNotContain} from "jstests/aggregation/extras/utils.js";

const coll = db.single_stage_alias_error;

coll.drop();

// Assert that, despite the fact $set and $addFields are internally identical, error messages
// use only the name used by the user.
let pipeline = [{"$set": {"a.$c": 1}}];
assertErrMsgContains(coll, pipeline, "$set");
assertErrMsgDoesNotContain(coll, pipeline, "$addFields");

pipeline = [{"$addFields": {"$a.$c": 1}}];
assertErrMsgContains(coll, pipeline, "$addFields");

// From version 5.0 on the error message suggests using $setField (which trivially
// includes $set) so this assert should check for something that isn't a substring of $setField.
assertErrMsgDoesNotContain(coll, pipeline, "$set ");

// Assert that, despite the fact $unset is an alias for an exclusion projection, error messages
// use only the name used by the user.
pipeline = [{"$unset": [""]}];
assertErrMsgContains(coll, pipeline, "$unset");
assertErrMsgDoesNotContain(coll, pipeline, "$project");

pipeline = [{"$project": [""]}];
assertErrMsgContains(coll, pipeline, "$project");
assertErrMsgDoesNotContain(coll, pipeline, "$unset");

// Assert that, despite the fact that $replaceWith is just an alias for $replaceRoot, error
// messages contain syntax that matches the documentation for whichever name the user inputs.
let doc = {"_id": 0};
coll.insert(doc);
pipeline = [{"$replaceWith": "abc"}];
