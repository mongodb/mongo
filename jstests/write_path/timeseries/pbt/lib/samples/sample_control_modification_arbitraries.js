/**
 * This is a sampling test of measurement streams that update control..
 * It can be run with the no_passthrough suite to test.
 */

const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

import {describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";
import {makeMeasurementDocStreamArb} from "jstests/write_path/timeseries/pbt/lib/measurement_arbitraries.js";

describe("Arbitraries repeat control-modifying doc updates", () => {
    it("creates measurement batch that updates control", () => {
        const samples = fc.sample(
            makeMeasurementDocStreamArb(timeField, metaField, metaValue, {
                extendControlFrequency: 0.9,
                newFieldFrequency: 0.5,
            }),
            15,
        );
        jsTest.log.info({samples});
    });
});
