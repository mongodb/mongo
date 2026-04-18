/**
 * Tests that the validate command uses distinct error/warning messages for time-series bucket
 * schema violations vs user-defined schema violations.
 *
 * @tags: [requires_fcv_90]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const testDB = db.getSiblingDB(jsTestName());

describe("validate distinguishes time-series bucket vs user-defined schema violations", function () {
    afterEach(function () {
        assert.commandWorked(testDB.adminCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: false}));
        testDB.dropDatabase();
    });

    it("reports time-series-specific error for corrupted bucket", function () {
        assert.commandWorked(testDB.createCollection("ts_coll", {timeseries: {timeField: "t"}}));
        const coll = testDB.getCollection("ts_coll");

        coll.insertOne({t: ISODate()});

        // Mark a compressed bucket as uncompressed so it fails the strict consistency check.
        assert.commandWorked(testDB.adminCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: true}));
        getTimeseriesCollForRawOps(testDB, coll).update(
            {},
            {$set: {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
            getRawOperationSpec(testDB),
        );
        assert.commandWorked(testDB.adminCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: false}));

        const res = assert.commandWorked(coll.validate());

        assert(!res.valid, tojson(res));
        assert.gt(res.nNonCompliantDocuments, 0, tojson(res));

        assert(
            res.errors.some((e) => e.includes("time-series specifications")),
            "Expected 'time-series specifications' in error, got: " + tojson(res.errors),
        );
        assert(
            res.errors.some((e) => e.includes("11634800")),
            "Expected log id 11634800 in error, got: " + tojson(res.errors),
        );
        assert(
            !res.errors.some((e) => e.includes("5363500")),
            "Should not reference user-schema log id 5363500, got: " + tojson(res.errors),
        );
    });

    it("reports generic schema warning for user-defined schema violation", function () {
        assert.commandWorked(
            testDB.createCollection("regular_coll", {
                validator: {$jsonSchema: {required: ["name"]}},
                validationAction: "warn",
            }),
        );

        assert.commandWorked(
            testDB.runCommand({insert: "regular_coll", documents: [{x: 1}], bypassDocumentValidation: true}),
        );

        const res = assert.commandWorked(testDB.getCollection("regular_coll").validate());

        assert(res.valid, tojson(res));
        assert.gt(res.nNonCompliantDocuments, 0, tojson(res));

        assert(
            res.warnings.some((w) => w.includes("collection's schema")),
            "Expected generic schema warning, got: " + tojson(res.warnings),
        );
        assert(
            res.warnings.some((w) => w.includes("5363500")),
            "Expected log id 5363500 in warning, got: " + tojson(res.warnings),
        );
        assert(
            !res.warnings.some((w) => w.includes("time-series")),
            "Should not mention time-series for user schema violation, got: " + tojson(res.warnings),
        );
    });
});
