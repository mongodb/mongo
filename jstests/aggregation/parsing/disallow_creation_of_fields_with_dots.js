// do not allow creation of fields with a $ prefix
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const c = db.c;
c.drop();

assert.commandWorked(c.insert({a: 1}));

// assert that we get the proper error in both $project and $group
assertErrorCode(c, {$project: {$a: "$a"}}, 16410);
assertErrorCode(c, {$project: {a: {$b: "$a"}}}, 31325);
assertErrorCode(c, {$project: {a: {"$b": "$a"}}}, 31325);
assertErrorCode(c, {$project: {"a.$b": "$a"}}, 16410);
assertErrorCode(c, {$group: {_id: "$_id", $a: {$sum: 1}}}, 40236);
assertErrorCode(c, {$group: {_id: {$a: "$a"}}}, ErrorCodes.InvalidPipelineOperator);
