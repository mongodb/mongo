/**
 * Tests that a failed time-series insert does not leave behind any invalid state.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();

const testDB = conn.getDB(jsTestName());

const coll = testDB.getCollection("t");

const timeFieldName = "time";
const metaFieldName = "m";

const resetColl = function () {
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );
};

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 0},
];

const runTest = function (ordered) {
    jsTestLog("Running test with {ordered: " + ordered + "} inserts");
    resetColl();

    const fp1 = configureFailPoint(conn, "failAtomicTimeseriesWrites");
    const fp2 = configureFailPoint(conn, "failUnorderedTimeseriesInsert", {metadata: 0});

    assert.commandFailed(coll.insert(docs[0], {ordered: ordered}));

    fp1.off();
    fp2.off();

    // Insert a document that belongs in the same bucket that the failed insert would have gone
    // into.
    assert.commandWorked(coll.insert(docs[1], {ordered: ordered}));

    // There should not be any leftover state from the failed insert.
    assert.docEq([docs[1]], coll.find().toArray());
    const buckets = getTimeseriesCollForRawOps(testDB, coll)
        .find()
        .rawData()
        .sort({["control.min." + timeFieldName]: 1})
        .toArray();
    jsTestLog("Checking buckets: " + tojson(buckets));
    assert.eq(buckets.length, 1);
    assert.eq(buckets[0].control.min._id, docs[1]._id);
};

runTest(true);
runTest(false);

MongoRunner.stopMongod(conn);
