/**
 * Verifies that showRecordId() returns the ObjectId type for time-series collections.
 *
 * @tags: [
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const timeFieldName = "time";

    const coll = db.timeseries_show_record_id;
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    Random.setRandomSeed();

    const numHosts = 10;
    const hosts = TimeseriesTest.generateHosts(numHosts);

    for (let i = 0; i < 100; i++) {
        const host = TimeseriesTest.getRandomElem(hosts);
        TimeseriesTest.updateUsages(host.fields);

        assert.commandWorked(insert(coll, {
            measurement: "cpu",
            time: ISODate(),
            fields: host.fields,
            tags: host.tags,
        }));
    }

    function checkRecordId(documents) {
        for (const document of documents) {
            assert(document.hasOwnProperty("$recordId"));
            assert(isString(document["$recordId"]));
        }
    }

    // The time-series user view uses aggregation to build a representation of the data.
    // showRecordId() is not support in aggregation.
    const error = assert.throws(() => {
        coll.find().showRecordId().toArray();
    });
    assert.commandFailedWithCode(error, ErrorCodes.InvalidPipelineOperator);

    const bucketsColl = db.getCollection("system.buckets." + coll.getName());
    checkRecordId(bucketsColl.find().showRecordId().toArray());
});
})();
