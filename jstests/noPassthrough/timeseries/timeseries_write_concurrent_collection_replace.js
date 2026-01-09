/**
 * Tests that dropping and re-creating a collection will interrupt time-series operations that have
 * already prepared.
 *
 * @tags: [
 *  requires_fcv_81,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isFCVlt} from "jstests/libs/feature_compatibility_version.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const timeField = "t";
const coll = db.coll;

function runInsertTest(ordered, updateBucket, expectedErrorCodes) {
    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(
        db.createCollection(coll.getName(), {
            timeseries: {timeField: timeField},
        }),
    );

    if (updateBucket) {
        // Causes the subsequent hanging write to execute as a bucket update.
        assert.commandWorked(coll.insert([{t: new Date(), value: "test1"}]));
    }

    const fp = configureFailPoint(conn, "hangTimeseriesInsertBeforeWrite");

    const insertShell = startParallelShell(
        funWithArgs(
            (dbName, collName, ordered, errorCodes) => {
                assert.commandFailedWithCode(
                    db
                        .getSiblingDB(dbName)
                        .getCollection(collName)
                        .insert([{t: new Date(), value: "test2"}], {ordered: ordered}),
                    errorCodes,
                );
            },
            db.getName(),
            coll.getName(),
            ordered,
            expectedErrorCodes,
        ),
        conn.port,
    );

    fp.wait();

    assert(coll.drop());
    assert.commandWorked(
        db.createCollection(coll.getName(), {
            timeseries: {timeField: timeField},
        }),
    );

    fp.off();
    insertShell();

    assert(coll.drop());
}
// TODO SERVER-101784 remove the legacy error code once 9.0 becomes last LTS
const canThrowLegacyCodes = isFCVlt(db, "8.3");

runInsertTest(false, false, canThrowLegacyCodes ? [9748801, 10685101] : [10685101]);
runInsertTest(false, true, canThrowLegacyCodes ? [9748802, 10685101] : [10685101]);
runInsertTest(true, false, canThrowLegacyCodes ? [9748800, 10685101] : [10685101]);
runInsertTest(true, true, canThrowLegacyCodes ? [9748800, 10685101] : [10685101]);

MongoRunner.stopMongod(conn);
