/**
 *  Tests behavior with invalid 2d bounds.
 *
 * @tags: [
 *   requires_fcv_83
 *  ]
 */

import {GeoNearRandomTest} from "jstests/libs/query/geo_near_random.js";

const test = new GeoNearRandomTest("geo_near_bounds_overflow");

function testBounds(min, max, expectedErrorCode = null) {
    const indexBounds = {min: min, max: max};
    test.insertPts(50, indexBounds, 1, true);

    if (expectedErrorCode === null) {
        assert.commandWorked(test.t.createIndex({loc: "2d"}, indexBounds));
    } else {
        assert.commandFailedWithCode(test.t.createIndex({loc: "2d"}, indexBounds), expectedErrorCode);
    }

    test.reset();
}

// Test max = Inf.
testBounds(-Math.pow(2, 34), Math.pow(-2147483648, 34), ErrorCodes.InvalidOptions);

// Test min = -Inf.
testBounds(-Math.pow(-2147483648, 34), 1, ErrorCodes.InvalidOptions);

// Test min = -Inf and max = Inf.
testBounds(-Math.pow(-2147483648, 34), Math.pow(-2147483648, 34), ErrorCodes.InvalidOptions);

// Test min = largest possible negative value and max = largest possible value (Not infinity).
// Because scaling = buckets / (max - min), the denominator here overflows to Infinity, which is invalid.
testBounds(-Number.MAX_VALUE, Number.MAX_VALUE, ErrorCodes.InvalidOptions);

// Test min = Nan.
testBounds(0 / 0, 1, ErrorCodes.InvalidOptions);

// Test max = Nan.
testBounds(1, 0 / 0, ErrorCodes.InvalidOptions);

// Test min and max = Nan.
testBounds(0 / 0, 0 / 0, ErrorCodes.InvalidOptions);

// Test min > max.
testBounds(1, -1, ErrorCodes.InvalidOptions);

// Test min and max very close together.
// Because scaling = buckets / (max - min), the extremely small denominator causes scaling = Inf.
testBounds(0, Number.MIN_VALUE, ErrorCodes.InvalidOptions);

// Test xtremely large positive, non-infinity max
testBounds(0, Number.MAX_VALUE);

// Test extremely large negative, non-infinity min
testBounds(-Number.MAX_VALUE, 0);
