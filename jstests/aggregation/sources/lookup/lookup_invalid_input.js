/**
 * Tests for invalid and valid combinations of pipeline, localField, and foreignField in $lookup.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.lookup_invalid_input;
const from = db.lookup_invalid_input_from;

coll.drop();
from.drop();
assert.commandWorked(coll.insert({_id: 1, x: 1}));
assert.commandWorked(from.insert({_id: 1, y: 1}));

// Tests with localField and foreignField but no pipeline.
assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$lookup: {from: from.getName(), localField: "x", foreignField: "y", as: "result"}}],
        cursor: {},
    }),
);

// Tests with only pipeline.
assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$lookup: {from: from.getName(), pipeline: [{$match: {y: 1}}], as: "result"}}],
        cursor: {},
    }),
);

// Tests with pipeline and both localField and foreignField.
assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {
                $lookup: {
                    from: from.getName(),
                    localField: "x",
                    foreignField: "y",
                    pipeline: [{$match: {y: 1}}],
                    as: "result",
                },
            },
        ],
        cursor: {},
    }),
);

// Checks that the error message mentions 'pipeline' when neither localField nor foreignField is provided.
const res = assert.commandFailedWithCode(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$lookup: {from: from.getName(), as: "result"}}],
        cursor: {},
    }),
    ErrorCodes.FailedToParse,
);
assert(res.errmsg.includes("pipeline"), "unexpected error message: " + res.errmsg);

// Missing foreignField.
assertErrorCode(coll, [{$lookup: {from: from.getName(), localField: "x", as: "result"}}], ErrorCodes.FailedToParse);

// Missing localField.
assertErrorCode(coll, [{$lookup: {from: from.getName(), foreignField: "y", as: "result"}}], ErrorCodes.FailedToParse);

// Missing foreignField but has pipeline.
assertErrorCode(
    coll,
    [{$lookup: {from: from.getName(), localField: "x", pipeline: [{$match: {y: 1}}], as: "result"}}],
    ErrorCodes.FailedToParse,
);

// Has pipeline but is missing localField.
assertErrorCode(
    coll,
    [{$lookup: {from: from.getName(), foreignField: "y", pipeline: [{$match: {y: 1}}], as: "result"}}],
    ErrorCodes.FailedToParse,
);

// Missing 'from' field.
assertErrorCode(coll, [{$lookup: {localField: "x", foreignField: "y", as: "result"}}], ErrorCodes.FailedToParse);

// The 'let' specified without pipeline.
assertErrorCode(
    coll,
    [{$lookup: {from: from.getName(), let: {xVar: "$x"}, localField: "x", foreignField: "y", as: "result"}}],
    ErrorCodes.FailedToParse,
);
