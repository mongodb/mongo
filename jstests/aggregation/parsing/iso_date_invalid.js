// The $isoDate operator is not available.  SERVER-6290

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insertOne({}));

// $isoDate is an invalid operator.
assertErrorCode(t, {$project: {a: {$isoDate: [{year: 1}]}}}, 31325);
// $date is an invalid operator.
assertErrorCode(t, {$project: {a: {$date: [{year: 1}]}}}, 31325);

// Alternative operands.
assertErrorCode(t, {$project: {a: {$isoDate: []}}}, 31325);
assertErrorCode(t, {$project: {a: {$date: []}}}, 31325);
assertErrorCode(t, {$project: {a: {$isoDate: 'foo'}}}, 31325);
assertErrorCode(t, {$project: {a: {$date: 'foo'}}}, 31325);

// Test with $group.
assertErrorCode(t,
                {$group: {_id: 0, a: {$first: {$isoDate: [{year: 1}]}}}},
                ErrorCodes.InvalidPipelineOperator);
assertErrorCode(
    t, {$group: {_id: 0, a: {$first: {$date: [{year: 1}]}}}}, ErrorCodes.InvalidPipelineOperator);
