/**
 * Tests the impact of time-series metadata normalization on queries. Querying on the entire
 * metafield will not match any documents when the query metadata subfield ordering differs from the
 * normalized ordering.
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const coll = db[jsTestName()];
    const timeFieldName = "time";
    const metaFieldName = "metaField";

    function runTest(measurementsToInsert, queryFilter, expectedUnsuccessfulQueryFilter) {
        coll.drop();
        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        for (let i = 0; i < measurementsToInsert.length; i++) {
            assert.commandWorked(insert(coll, measurementsToInsert[i]));
        }

        // Querying without a filter, and querying with the metadata when the subfields are in
        // normalized order, should return all documents.
        assert.eq(coll.find().itcount(), measurementsToInsert.length);
        assert.eq(coll.find(queryFilter).itcount(), measurementsToInsert.length);

        // When querying on an object metaField value, if we query on the non-normalized field
        // ordering and the non-normalized ordering is different from the normalized ordering, we
        // will not return the documents. This is documented (somewhat) here:
        // https://www.mongodb.com/docs/manual/core/timeseries/timeseries-best-practices/#query-the-metafield-on-sub-fields
        assert.eq(coll.find(expectedUnsuccessfulQueryFilter).itcount(), 0);
    }

    // All subfields in normalized order
    runTest(/* measurementsToInsert = */
            [
                {
                    _id: 0,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {a: 1, b: 1}
                },
                {
                    _id: 1,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {a: 1, b: 1}
                },
                {
                    _id: 2,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {a: 1, b: 1}
                }
            ],
            /* queryFilter = */ {[metaFieldName]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQuery= */ {[metaFieldName]: {b: 1, a: 1}});

    // Mixed ordering
    runTest(/* measurementsToInsert = */
            [
                {
                    _id: 0,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {a: 1, b: 1}
                },
                {
                    _id: 1,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {b: 1, a: 1}
                },
                {
                    _id: 2,
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {b: 1, a: 1}
                }
            ],
            /* queryFilter = */ {[metaFieldName]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQuery=*/ {[metaFieldName]: {b: 1, a: 1}});

    // No subfields in normalized order
    runTest(/* measurementsToInsert = */
            [
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {b: 1, a: 1}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {b: 1, a: 1}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {b: 1, a: 1}
                }
            ],
            /* queryFilter = */ {[metaFieldName]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQuery=*/ {[metaFieldName]: {b: 1, a: 1}});

    // All subfields in normalized order, nested object
    runTest(/* measurementsToInsert = */
            [
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {a: 1, b: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {a: 1, b: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {a: 1, b: 1}}
                }
            ],
            /* queryFilter = */ {[metaFieldName + ".nested"]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQueryFilter=*/ {[metaFieldName + ".nested"]: {b: 1, a: 1}});

    // Mixed ordering, nested object
    runTest(/* measurementsToInsert = */
            [
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {a: 1, b: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {b: 1, a: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {b: 1, a: 1}}
                }
            ],
            /* queryFilter = */ {[metaFieldName + ".nested"]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQueryFilter=*/ {[metaFieldName + ".nested"]: {b: 1, a: 1}});

    // No subfields in normalized order, nested object
    runTest(/* measurementsToInsert = */
            [
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {b: 1, a: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {b: 1, a: 1}}
                },
                {
                    [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
                    [metaFieldName]: {nested: {b: 1, a: 1}}
                }
            ],
            /* queryFilter = */ {[metaFieldName + ".nested"]: {a: 1, b: 1}},
            /* expectedUnsuccessfulQueryFilter=*/ {[metaFieldName + ".nested"]: {b: 1, a: 1}});
})
