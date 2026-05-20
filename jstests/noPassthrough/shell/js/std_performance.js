import {describe, it} from "jstests/libs/mochalite.js";
import {performance} from "std:performance";

describe("std:performance with performance internal binding", function () {
    it("uses a monotonic high-resolution clock", function () {
        const first = performance.now();
        const second = performance.now();

        assert.gte(first, 0, "performance.now() should be non-negative");
        assert.gte(second, first, "performance.now() should be monotonic");

        let sawFractionalValue = false;
        for (let i = 0; i < 20000; ++i) {
            const sample = performance.now();
            if (!Number.isInteger(sample)) {
                sawFractionalValue = true;
                break;
            }
        }
        assert(sawFractionalValue, "performance.now() should report sub-millisecond precision");
    });
});
