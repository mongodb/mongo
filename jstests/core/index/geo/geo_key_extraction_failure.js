/**
 * Verifies structured GeoKeyExtractionFailureInfo on the insert path for a 2dsphere index: the
 * writeError wire shape, with the structured fields (failingPath, underlyingCode, underlyingReason,
 * failingElement) nested under errInfo so drivers surface them to users as details.
 *
 * @tags: [
 *   # The structured writeError fields are new in this version.
 *   requires_fcv_90,
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

function assertWriteError(we, opts) {
    const {
        code = ErrorCodes.GeoKeyExtractionFailed,
        failingPath,
        errmsgContains = [],
        errmsgForbids = [],
        maxErrmsgLength = 1024,
        underlyingReasonContains = [],
        underlyingReasonEndsWith,
        maxUnderlyingReasonLength,
        failingElement,
    } = opts;

    assert.eq(we.code, code, "writeError code mismatch", {we});

    // Structured fields are nested under errInfo so drivers surface them to users as details.
    const errInfo = we.errInfo;
    assert.neq(errInfo, undefined, "expected errInfo on writeError", {we});

    if (failingPath !== undefined) {
        assert.eq(errInfo.failingPath, failingPath, "failingPath mismatch", {we});
    }

    assert(we.errmsg.length < maxErrmsgLength, "errmsg over limit", {
        errmsgLength: we.errmsg.length,
        errmsg: we.errmsg,
    });

    for (const fragment of [].concat(errmsgContains)) {
        assert(we.errmsg.includes(fragment), "expected fragment in errmsg", {
            fragment,
            errmsg: we.errmsg,
        });
    }

    for (const forbidden of [].concat(errmsgForbids)) {
        assert(!we.errmsg.includes(forbidden), "errmsg contains forbidden content", {
            forbidden,
            errmsg: we.errmsg,
        });
    }

    for (const fragment of [].concat(underlyingReasonContains)) {
        assert(
            errInfo.underlyingReason.includes(fragment),
            "expected fragment in underlyingReason",
            {
                fragment,
                underlyingReason: errInfo.underlyingReason,
            },
        );
    }

    if (underlyingReasonEndsWith !== undefined) {
        assert(
            errInfo.underlyingReason.endsWith(underlyingReasonEndsWith),
            "underlyingReason should end with marker",
            {
                underlyingReason: errInfo.underlyingReason,
            },
        );
    }

    if (maxUnderlyingReasonLength !== undefined) {
        assert(
            errInfo.underlyingReason.length < maxUnderlyingReasonLength,
            "underlyingReason over limit",
            {
                underlyingReasonLength: errInfo.underlyingReason.length,
                underlyingReason: errInfo.underlyingReason,
            },
        );
    }

    if (failingElement !== undefined) {
        for (const [key, value] of Object.entries(failingElement)) {
            assert.eq(errInfo.failingElement[key], value, `failingElement.${key} mismatch`, {
                failingElement: errInfo.failingElement,
            });
        }
    }
}

describe("Geo key-extraction structured ExtraInfo (insert path)", function () {
    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.createIndex({"features.geometry": "2dsphere"}, {"2dsphereIndexVersion": 3}),
        );
    });

    it("attaches structured ExtraInfo with bounded errmsg", function () {
        const we = insertExpectingFailure({
            features: [{geometry: {type: "Point", coordinates: ["bad", 0]}}],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            errmsgContains: ["features.geometry", "Could not extract geo keys"],
            errmsgForbids: ["Can't extract geo keys", "coordinates"],
            failingElement: {type: "Point", coordinates: ["bad", 0]},
        });
        assert.gt(we.errInfo.underlyingCode, 0, "underlyingCode populated", {we});
        assert.gt(we.errInfo.underlyingReason.length, 0, "underlyingReason populated", {we});
    });

    it("multikey expansion isolates the failing leaf", function () {
        const we = insertExpectingFailure({
            features: [
                {geometry: {type: "Point", coordinates: [0, 0]}},
                {geometry: {type: "Point", coordinates: ["bad", 0]}},
                {geometry: {type: "Point", coordinates: [1, 1]}},
            ],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            failingElement: {type: "Point", coordinates: ["bad", 0]},
        });
    });

    it("preserves full failingElement while bounding underlyingReason", function () {
        // Unclosed loop (last vertex != first); the 500-vertex size triggers reason bounding.
        const loop = Array.from({length: 500}, (_, i) => [i * 0.001, 0]);

        const we = insertExpectingFailure({
            features: [{geometry: {type: "Polygon", coordinates: [loop]}}],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            maxUnderlyingReasonLength: 512,
            underlyingReasonEndsWith: "...",
            failingElement: {type: "Polygon"},
        });
        assert.eq(
            we.errInfo.failingElement.coordinates[0].length,
            500,
            "failingElement should carry full coords",
            {
                coordLength: we.errInfo.failingElement.coordinates[0].length,
            },
        );
    });

    it("reports missing 'type' field", function () {
        const we = insertExpectingFailure({
            features: [{geometry: {coordinates: [0, 0]}}],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            underlyingReasonContains: "type field missing or not a string",
        });
    });

    it("reports wrong-type 'type' field", function () {
        // V3 dispatch picks the legacy-point parser if the first field is numeric, so put
        // a non-numeric field first to hit the GeoJSON unknown-type branch.
        const we = insertExpectingFailure({
            features: [{geometry: {coordinates: [0, 0], type: 42}}],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            underlyingReasonContains: "type field missing or not a string",
        });
    });

    it("reports non-object geo element", function () {
        const we = insertExpectingFailure({
            features: [{geometry: "scalar-not-an-object"}],
        });
        assertWriteError(we, {
            failingPath: "features.geometry",
            underlyingReasonContains: "got string",
        });
    });
});
