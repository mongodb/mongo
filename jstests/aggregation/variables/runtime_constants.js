/*
 * Tests the behavior of runtime constants $$IS_MR and $$JS_SCOPE.
 */

import "jstests/libs/sbe_assert_error_override.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.runtime_constants;
coll.drop();

assert.commandWorked(coll.insert({x: true}));

// Runtime constant $$IS_MR is unable to be retrieved by users.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$addFields: {testField: "$$IS_MR"}}], cursor: {}}),
    [51144]);

// Runtime constant $$JS_SCOPE is unable to be retrieved by users.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$addFields: {field: "$$JS_SCOPE"}}], cursor: {}}),
    [51144]);

// Tests that runtimeConstants can't be specified on a mongod if fromMongos is false.
const rtc = {
    localNow: new Date(),
    clusterTime: new Timestamp(0, 0),
};

if (!FixtureHelpers.isMongos(db)) {
    // RuntimeConstants is disallowed when fromMongos is false.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$project: {_id: 0}}],
        cursor: {},
        runtimeConstants: rtc,
        fromMongos: false
    }),
                                 463840);
    // RuntimeConstants is allowed when fromMongos is true.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$project: {_id: 0}}],
        cursor: {},
        runtimeConstants: rtc,
        fromMongos: true
    }));
}
