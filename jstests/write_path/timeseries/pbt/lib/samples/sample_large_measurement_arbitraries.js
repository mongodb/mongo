/**
 * This is a sampling test of large measurement arbitraries.
 * It can be run with the no_passthrough suite to test large string creation.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {makeLargeStringArb} from "jstests/write_path/timeseries/pbt/lib/large_measurement_arbitraries.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

describe("Large arbitraries at or above the BSON limit", () => {
    it("makeLargeStringArb creates large string arbitraries", () => {
        const samples = fc.sample(makeLargeStringArb());
        const reducedForLog = samples.map((s) => {
            const partialLength = 100;
            // If it's too long, truncate it.
            if (s.length > partialLength * 3) {
                return `${s.slice(0, partialLength)}[....${s.length - partialLength * 2} characters hidden....]${s.slice(-partialLength)}`;
            }
            return s;
        });
        const lengths = samples.map((s) => s.length);
        const averageLength = lengths.reduce((acc, l) => acc + l, 0) / samples.length;
        const maxLength = Math.max(...lengths);
        jsTest.log.info({samples: reducedForLog, averageLength, maxLength});
    });
});
