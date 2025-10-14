/**
 * Tests that $merge fails when a timeseries collection is the target collection.
 * This is because $merge requires a unique index on the "on" field(s) and timeseries
 * collections do not support unique indexes.
 *
 * @tags: [
 *      requires_fcv_83,
 *      requires_timeseries
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const sourceColl = jsTestName() + "_inColl";
const targetColl = jsTestName() + "_timeseries";
const timeFieldName = "time";

(function dropCollections() {
    db.getCollection(sourceColl).drop();
    db.getCollection(targetColl).drop();
})();

// Create a timeseries collection
assert.commandWorked(db.createCollection(targetColl, {timeseries: {timeField: timeFieldName}}));

// Insert a document into the source collection
assert.commandWorked(db.sourceColl.insert({field: "hello"}));

// When no "on" field is provided, $merge should fail due to the target collection being timeseries.
assert.throwsWithCode(
    () =>
        db.sourceColl.aggregate([
            {
                $merge: {
                    into: targetColl,
                },
            },
        ]),
    1074330,
);

// TODO SERVER-108560: Remove this if statement once 9.0 becomes last LTS and there are only viewless timeseries.
if (areViewlessTimeseriesEnabled(db) || !FixtureHelpers.isMongos(db)) {
    assert.throwsWithCode(
        () =>
            db.sourceColl.aggregate([
                {
                    $merge: {
                        on: "_id",
                        into: targetColl,
                    },
                },
            ]),
        // For sharded & tracked unsharded collections in a sharded cluster this will fail on mongos due to
        // _id not existing as a unique index (51990). Otherwise this will fail at the mongod level because
        // targetColl is a timeseries collection (1074330).
        [51190, 1074330],
    );
}

// Create a secondary non-unique index on the field we will merge on.
assert.commandWorked(db.targetColl.createIndex({[timeFieldName]: 1}));

// Insert a document into the timeseries collection.
assert.commandWorked(db.targetColl.insert({[timeFieldName]: ISODate(), a: 1}));

// TODO SERVER-108560: Remove this if statement once 9.0 becomes last LTS and there are only viewless timeseries.
if (areViewlessTimeseriesEnabled(db) || !FixtureHelpers.isMongos(db)) {
    assert.throwsWithCode(
        () =>
            db.sourceColl.aggregate([
                {
                    $merge: {
                        on: timeFieldName,
                        into: targetColl,
                    },
                },
            ]),
        // For sharded & tracked unsharded collections in a sharded cluster this will fail on mongos due to
        // "time" not existing as a unique index (51990). Otherwise this will fail at the mongod level because
        // targetColl is a timeseries collection (1074330).
        [51190, 1074330],
    );
}
