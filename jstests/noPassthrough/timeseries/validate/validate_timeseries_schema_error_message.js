/**
 * Tests that the validate command uses distinct error/warning messages for time-series bucket
 * schema violations vs user-defined schema violations.
 *
 * @tags: [requires_fcv_90]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

describe("validate distinguishes time-series bucket vs user-defined schema violations", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(jsTestName());
    });

    after(function () {
        // regular_coll intentionally leaves a non-compliant document; skip shutdown validation.
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });

    it("reports time-series-specific error for corrupted bucket", function () {
        const collName = "ts_coll";
        assert.commandWorked(this.db.createCollection(collName, {timeseries: {timeField: "t"}}));
        const coll = this.db.getCollection(collName);

        coll.insertOne({t: ISODate()});

        // Mark a compressed bucket as uncompressed so it fails the strict consistency check.
        assert.commandWorked(
            this.conn.getDB("admin").runCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: true}),
        );
        getTimeseriesCollForRawOps(this.db, coll).update(
            {},
            {$set: {"control.version": TimeseriesTest.BucketVersion.kUncompressed}},
            getRawOperationSpec(this.db),
        );
        assert.commandWorked(
            this.conn.getDB("admin").runCommand({setParameter: 1, timeseriesDisableStrictBucketValidator: false}),
        );

        const res = assert.commandWorked(coll.validate());

        assert(!res.valid, res);
        assert.eq(res.nNonCompliantDocuments, 1, res);

        assert(
            res.errors.some((e) => e.includes("time-series specifications")),
            "Expected 'time-series specifications' in errors, got: " + tojson(res.errors),
        );
        assert(
            res.errors.some((e) => e.includes("11634800")),
            "Expected log id 11634800 in errors, got: " + tojson(res.errors),
        );
        assert(
            !res.errors.some((e) => e.includes("5363500")),
            "Should not reference user-schema log id 5363500, got: " + tojson(res.errors),
        );
    });

    it("reports generic schema warning for user-defined schema violation", function () {
        const collName = "regular_coll";
        assert.commandWorked(
            this.db.createCollection(collName, {
                validator: {$jsonSchema: {required: ["name"]}},
                validationAction: "warn",
            }),
        );

        assert.commandWorked(
            this.db.runCommand({insert: collName, documents: [{x: 1}], bypassDocumentValidation: true}),
        );

        const res = assert.commandWorked(this.db.getCollection(collName).validate());

        assert(res.valid, res);
        assert.eq(res.nNonCompliantDocuments, 1, res);

        assert(
            res.warnings.some((w) => w.includes("collection's schema")),
            "Expected generic schema warning, got: " + tojson(res.warnings),
        );
        assert(
            res.warnings.some((w) => w.includes("5363500")),
            "Expected log id 5363500 in warnings, got: " + tojson(res.warnings),
        );
        assert(
            !res.warnings.some((w) => w.includes("time-series")),
            "Should not mention time-series for user schema violation, got: " + tojson(res.warnings),
        );
    });
});
