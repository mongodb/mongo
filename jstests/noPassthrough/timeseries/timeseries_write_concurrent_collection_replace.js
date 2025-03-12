/**
 * Tests that dropping and re-creating a collection will interrupt time-series operations that have
 * already prepared.
 *
 * @tags: [
 *  requires_fcv_81,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

const timeField = "t";
const coll = db.coll;

function runInsertTest(ordered, updateBucket, expectedErrorCode) {
    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeField},
    }));

    if (updateBucket) {
        // Causes the subsequent hanging write to execute as a bucket update.
        assert.commandWorked(coll.insert([{t: new Date(), value: "test1"}]));
    }

    const fp = configureFailPoint(conn, "hangTimeseriesInsertBeforeWrite");

    const insertShell = startParallelShell(
        funWithArgs((dbName, collName, ordered, errorCode) => {
            assert.commandFailedWithCode(db.getSiblingDB(dbName).getCollection(collName).insert(
                                             [{t: new Date(), value: "test2"}], {ordered: ordered}),
                                         errorCode);
        }, db.getName(), coll.getName(), ordered, expectedErrorCode), conn.port);

    fp.wait();

    assert(coll.drop());
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeField},
    }));

    fp.off();
    insertShell();

    assert(coll.drop());
}

runInsertTest(false, false, 9748801);
runInsertTest(false, true, 9748802);
runInsertTest(true, false, 9748800);
runInsertTest(true, true, 9748800);

MongoRunner.stopMongod(conn);
