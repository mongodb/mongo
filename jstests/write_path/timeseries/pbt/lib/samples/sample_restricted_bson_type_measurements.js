/**
 * This is a sampling test of measurements with restricted types.
 * It can be run with the no_passthrough suite to test.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";
import {
    makeMeasurementDocArb,
    makeMeasurementDocStreamArb,
} from "jstests/write_path/timeseries/pbt/lib/measurement_arbitraries.js";

const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

describe("Restricted BSON types", () => {
    it("creates measurements with restricted BSON types 'double' and 'decimal'", () => {
        const samples = fc.sample(
            makeMeasurementDocArb(timeField, metaField, metaValue, 5, 5, {types: ["double", "decimal"]}),
            30,
        );
        jsTest.log.info({samples});
    });

    it("creates measurement streams with restricted BSON types 'int', 'long', and 'bool'", () => {
        const samples = fc.sample(
            makeMeasurementDocStreamArb(timeField, metaField, metaValue, {
                ranges: {maxFields: 5, minFields: 5, minDocs: 5, maxDocs: 5},
                types: ["int", "long", "bool"],
            }),
            15,
        );
        jsTest.log.info({samples});
    });
});
