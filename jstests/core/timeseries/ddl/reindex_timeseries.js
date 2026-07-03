/**
 * Tests that the reIndex command is rejected on timeseries collections.
 *
 * reIndex is a deprecated, standalone-only command. On a standalone it must explicitly fail on a
 * timeseries collection (which it would otherwise silently rebuild now that timeseries collections
 * are viewless). On a replica set it fails with the existing standalone-only guard, and it is not
 * registered on mongos.
 *
 * @tags: [
 *   # reIndex on a timeseries namespace was not failing on timeseries collections before SERVER-127051.
 *   backport_required_multiversion,
 *   # The test runs commands that are not allowed with security token: reIndex.
 *   not_allowed_with_signed_security_token,
 *   requires_timeseries,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());

const tsColl = testDB.ts;
const timeFieldName = "t";
tsColl.drop();
assert.commandWorked(
    testDB.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}),
);
assert.commandWorked(tsColl.insert({[timeFieldName]: ISODate()}));

if (FixtureHelpers.isMongos(testDB)) {
    // reIndex is registered .forShard() only, so it is not available on mongos.
    assert.commandFailedWithCode(tsColl.reIndex(), ErrorCodes.CommandNotFound);
} else if (FixtureHelpers.isReplSet(testDB)) {
    // reIndex is only allowed on a standalone mongod instance.
    assert.commandFailedWithCode(tsColl.reIndex(), ErrorCodes.IllegalOperation);
} else {
    // Standalone: reIndex is explicitly forbidden on timeseries collections.
    assert.commandFailedWithCode(tsColl.reIndex(), ErrorCodes.CommandNotSupported);

    // Sanity check: reIndex still works on a regular collection.
    const regularColl = testDB.regular;
    regularColl.drop();
    assert.commandWorked(regularColl.insert({a: 1}));
    assert.commandWorked(regularColl.createIndex({a: 1}));
    assert.commandWorked(regularColl.reIndex());
    regularColl.drop();

    // reIndex on a plain (non-timeseries) view still fails with CommandNotSupportedOnView.
    const viewName = "regularView";
    testDB[viewName].drop();
    assert.commandWorked(testDB.createView(viewName, regularColl.getName(), []));
    assert.commandFailedWithCode(testDB[viewName].reIndex(), ErrorCodes.CommandNotSupportedOnView);
    assert.commandWorked(testDB.runCommand({drop: viewName}));
}

tsColl.drop();
