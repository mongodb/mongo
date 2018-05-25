// Test server validation of snapshot history window related server parameters' settings on startup
// and via setParameter command.

(function() {
    'use strict';

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");

    // Valid parameter values are in the range [0, infinity).
    testNumericServerParameter("maxTargetSnapshotHistoryWindowInSeconds",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               0 /*defaultValue*/,
                               30 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               -1 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);

    // Valid parameter values are in the range [0, 100].
    testNumericServerParameter("cachePressureThreshold",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               50 /*defaultValue*/,
                               70 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               -1 /*lowerOutOfBounds*/,
                               true /*hasUpperBound*/,
                               101 /*upperOutOfBounds*/);

    // Valid parameter values are in the range (0, 1).
    testNumericServerParameter("snapshotWindowMultiplicativeDecrease",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               0.75 /*defaultValue*/,
                               0.50 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               -1 /*lowerOutOfBounds*/,
                               true /*hasUpperBound*/,
                               1.1 /*upperOutOfBounds*/);

    // Valid parameter values are in the range [1, infinity).
    testNumericServerParameter("snapshotWindowAdditiveIncreaseSeconds",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               2 /*defaultValue*/,
                               10 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);

    // Valid parameter values are in the range [1, infinity).
    testNumericServerParameter("checkCachePressurePeriodSeconds",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               5 /*defaultValue*/,
                               8 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);

    // Valid parameter values are in the range [1, infinity).
    testNumericServerParameter("minMillisBetweenSnapshotWindowInc",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               500 /*defaultValue*/,
                               2 * 1000 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);

    // Valid parameter values are in the range [1, infinity).
    testNumericServerParameter("minMillisBetweenSnapshotWindowDec",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               500 /*defaultValue*/,
                               2 * 1000 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);
})();
