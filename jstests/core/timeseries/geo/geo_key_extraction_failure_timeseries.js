/**
 * Verifies structured GeoKeyExtractionFailureTimeseriesInfo on the insert path for a 2dsphere
 * index on a timeseries collection: the GeoKeyExtractionFailedTimeseries writeError with the failing bucket-schema path,
 * for both a malformed point and a valid non-point measurement.
 *
 * @tags: [
 *   # The structured writeError fields are new in this version.
 *   requires_fcv_90,
 *   requires_timeseries,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

let coll;

function insertExpectingFailure(doc) {
    const res = coll.runCommand({insert: coll.getName(), documents: [doc]});
    assert.eq(res.ok, 1, "insert command failed", {res});
    assert(res.writeErrors && res.writeErrors.length === 1, "expected one writeError", {res});
    return res.writeErrors[0];
}

describe("Geo key-extraction structured ExtraInfo (timeseries insert path)", function () {
    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: "time"}}),
        );
        assert.commandWorked(coll.createIndex({"loc": "2dsphere"}, {"2dsphereIndexVersion": 3}));
    });

    it("attaches GeoKeyExtractionFailureTimeseriesInfo", function () {
        const we = insertExpectingFailure({
            time: new Date(),
            loc: {type: "Point", coordinates: ["bad", 0]},
        });
        assert.eq(
            we.code,
            ErrorCodes.GeoKeyExtractionFailedTimeseries,
            "writeError code mismatch",
            {we},
        );
        // On timeseries, failingPath reflects the bucket schema the index operates on
        // (data.<measurement>), not the user-facing measurement name.
        assert.eq(we.failingPath, "data.loc", "failingPath mismatch", {we});
        assert.eq(we.failingElement.type, "Point", "failingElement.type mismatch", {we});
        assert.gt(we.underlyingCode, 0, "underlyingCode populated", {we});
        assert.gt(we.underlyingReason.length, 0, "underlyingReason populated", {we});
    });

    it("reports a non-point geo measurement", function () {
        // Time-series 2dsphere indexes only support point data, so a valid non-point geometry is
        // reported with the measurement that failed.
        const we = insertExpectingFailure({
            time: new Date(),
            loc: {
                type: "LineString",
                coordinates: [
                    [0, 0],
                    [1, 1],
                ],
            },
        });
        assert.eq(
            we.code,
            ErrorCodes.GeoKeyExtractionFailedTimeseries,
            "writeError code mismatch",
            {we},
        );
        assert.eq(we.failingPath, "data.loc", "failingPath mismatch", {we});
        assert(we.underlyingReason.includes("only support point data"), "expected reason", {we});
        assert.eq(we.failingElement.type, "LineString", "failingElement.type mismatch", {we});
    });
});
