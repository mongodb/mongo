/**
 * This is a sampling test of geodata arbitraries.
 * It can be run with the no_passthrough suite to test GeoJSON point and query generation.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";
import {
    makeGeoPointArb,
    makeGeospatialQueryArb,
    makeLongLatArb,
} from "jstests/write_path/timeseries/pbt/geodata/geodata_arbitraries.js";

describe("Geodata arbitraries", () => {
    it("makeLongLatArb creates longitude/latitude coordinate pairs", () => {
        const samples = fc.sample(makeLongLatArb(), 20);
        jsTest.log.info({samples});
    });

    it("makeGeoPointArb creates GeoJSON Point objects", () => {
        const samples = fc.sample(makeGeoPointArb(), 20);
        jsTest.log.info({samples});
    });

    it("makeGeoPointArb creates GeoJSON Points with restricted ranges", () => {
        const samples = fc.sample(
            makeGeoPointArb({
                longitudeRange: {min: -5.0, max: 5.0},
                latitudeRange: {min: -5.0, max: 5.0},
            }),
            20,
        );
        jsTest.log.info({samples});
    });

    it("makeGeospatialQueryArb creates $geoWithin query specs", () => {
        const samples = fc.sample(makeGeospatialQueryArb("loc", 10_000), 20);
        jsTest.log.info({samples});
    });
});
