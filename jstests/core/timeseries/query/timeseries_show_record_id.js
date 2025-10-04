/**
 * Verifies that showRecordId() returns the ObjectId type for time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const timeFieldName = "time";

    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    Random.setRandomSeed();

    const numHosts = 10;
    const hosts = TimeseriesTest.generateHosts(numHosts);

    for (let i = 0; i < 100; i++) {
        const host = TimeseriesTest.getRandomElem(hosts);
        TimeseriesTest.updateUsages(host.fields);

        assert.commandWorked(
            insert(coll, {
                measurement: "cpu",
                time: ISODate(),
                fields: host.fields,
                tags: host.tags,
            }),
        );
    }

    function isRecordId(data) {
        return (
            isString(data) || // old format
            Object.prototype.toString.call(data) === "[object BinData]"
        );
    }

    function checkRecordId(documents) {
        for (const document of documents) {
            assert(document.hasOwnProperty("$recordId"));
            assert(isRecordId(document["$recordId"]));
        }
    }

    // The time-series measurements are generated on-the-fly by unpacking the underlying buckets,
    // so they do not have a record id.
    const error = assert.throws(() => {
        coll.find().showRecordId().toArray();
    });
    assert.commandFailedWithCode(error, ErrorCodes.InvalidPipelineOperator);

    checkRecordId(getTimeseriesCollForRawOps(coll).find().rawData().showRecordId().toArray());
});
