// Test timeseries-related server parameter settings on server startup

(function() {
'use strict';

load("jstests/noPassthrough/libs/server_parameter_helpers.js");

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('timeseriesBucketMaxCount',
                           true /*isStartupParameter*/,
                           false /*isRuntimeParameter*/,
                           1000 /*defaultValue*/,
                           100 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('timeseriesBucketMaxSize',
                           true /*isStartupParameter*/,
                           false /*isRuntimeParameter*/,
                           1024 * 125 /*defaultValue*/,
                           1024 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('timeseriesIdleBucketExpiryMemoryUsageThreshold',
                           true /*isStartupParameter*/,
                           false /*isRuntimeParameter*/,
                           1024 * 1024 * 100 /*defaultValue*/,
                           1024 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);
})();
