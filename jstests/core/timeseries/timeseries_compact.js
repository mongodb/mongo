/**
 * Tests that the regular collection compact command can be run against a time-series collection.
 *
 * @tags: [
 *   # Compact is not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   multiversion_incompatible,
 *   # The test runs commands that are not allowed with security token: compact.
 *   not_allowed_with_signed_security_token,
 *   # Cannot compact when using the in-memory storage engine.
 *   requires_persistence,
 *   requires_timeseries,
 *   uses_compact,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run(() => {
    const coll = db.timeseries_compact;
    coll.drop();

    const timeFieldName = "time";
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    const numDocs = 5;
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), x: i}));
    }

    // The compact command can be successful or interrupted because of cache pressure or
    // concurrent calls to compact.
    let res = db.runCommand({compact: coll.getName(), force: true});
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.Interrupted, tojson(res));
    res = db.runCommand({compact: "system.buckets." + coll.getName(), force: true});
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.Interrupted, tojson(res));
});
