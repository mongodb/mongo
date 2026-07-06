/**
 * Verify that a slot with a view of unowned Decimal128 correctly allocates an owned copy in
 * preparation for detaching the cursor. The test uses a time series collection to construct a query
 * that reads a metadata field multiple times from the same bucket, meaning it accesses the
 * ScanStage slot with that value in it across multiple getMore operations. Setting the batch size
 * to 1 ensures multiple batches and forces the cursor to detach and reattach after the first
 * measurement, invalidating the bucket's BSON buffer. In practice, the invalid buffer will almost
 * always remain readable, but ASAN builds are configured to move invalidated pages, which will
 * surface a sanitizer failure if the metadata slot maintains a dangling reference to the
 * invalidated page.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_getmore,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("Decimal128 metadata slot survives SBE detach/reattach between batches", function () {
    const timeField = "t";
    const metaField = "m";

    // Use a Decimal128 value that cannot be represented as a double, so it stays as Decimal128
    // through the full round-trip and is clearly distinguishable from any other type.
    const decimalMeta = NumberDecimal("1.23456789012345678901234567890123");

    before(function () {
        this.coll = assertDropAndRecreateCollection(db, jsTestName(), {
            timeseries: {timeField, metaField},
        });

        // All measurements share the same Decimal128 metadata value so they land in one bucket.
        // The ScanStage will read the bucket's single 'meta' BSON field into a Decimal128 slot
        // once, and that slot must remain valid across every inter-batch detach/reattach.
        const baseTime = new Date("2024-01-01T00:00:00Z");
        const docs = Array.from({length: 10}, (_, i) => ({
            [timeField]: new Date(baseTime.getTime() + i * 1000),
            [metaField]: decimalMeta,
            v: i,
        }));
        assert.commandWorked(this.coll.insertMany(docs));
    });

    it("returns correct Decimal128 metadata values after detach/reattach between batches", function () {
        // batchSize: 1 forces a detach/reattach after every document, so prepareForYielding() runs on the
        // ScanStage's Decimal128 slot nine times (once per inter-batch boundary) while the plan
        // is still mid-bucket.  If the slot is not properly owned before the detach/reattach, subsequent
        // reads will see garbage or crash in a debug build.
        //
        // The $match on a measurement field plus the inclusion $project (which covers the
        // metaField) is a pipeline shape that SBE lowers into a scan-unpack-project plan, causing
        // the ScanStage to populate the meta slot via placeFieldsFromRecordInAccessors().
        const results = this.coll
            .aggregate([{$match: {v: {$gte: 0}}}, {$project: {[metaField]: 1, v: 1, _id: 0}}], {
                cursor: {batchSize: 1},
            })
            .toArray();

        assert.eq(results.length, 10, "Expected 10 measurements", {results});
        for (const doc of results) {
            assert(doc[metaField].equals(decimalMeta), "Metadata value was corrupted after a yield", {
                doc,
                expected: decimalMeta,
            });
        }
    });
});
