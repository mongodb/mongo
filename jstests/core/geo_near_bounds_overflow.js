// Tests behavior with invalid 2d bounds.
(function() {
    "use strict";

    load('jstests/libs/geo_near_random.js');

    const test = new GeoNearRandomTest('geo_near_bounds_overflow');

    function testBounds(min, max) {
        const indexBounds = {min: min, max: max};
        test.insertPts(50, indexBounds, 1, true);

        // Handle case where either 1. indexLambda will fail but not throw. We are
        // asserting it
        // works, so the outer lambda generates an exception. 2. indexLambda itself
        // will throw.
        const indexLambda = function(t) {
            return t.ensureIndex({loc: '2d'}, indexBounds);
        };
        const assertLambda = function(t, lambda) {
            assert.commandWorked(lambda(t));
        };
        assert.throws(assertLambda, [test.t, indexLambda]);

        test.reset();
    }

    // Test max = Inf.
    testBounds(-Math.pow(2, 34), Math.pow(-2147483648, 34));

    // Test min = -Inf.
    testBounds(-Math.pow(-2147483648, 34), 1);

    // Test min = -Inf and max = Inf.
    testBounds(-Math.pow(-2147483648, 34), Math.pow(-2147483648, 34));

    // Test min = Nan.
    testBounds({min: 0 / 0, max: 1});

    // Test max = Nan.
    testBounds({min: 1, max: 0 / 0});

    // Test min and max = Nan.
    testBounds({min: 0 / 0, max: 0 / 0});

    // Test min > max.
    testBounds({min: 1, max: -1});

    // Test min and max very close together.
    testBounds({min: 0, max: 5.56268e-309});
}());