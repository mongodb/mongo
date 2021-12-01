/*
 * Tests time-series server parameter settings on server startup.

 * @tags: [
 *   requires_replication
 * ]
 */

(function() {
'use strict';

load("jstests/core/timeseries/libs/timeseries.js");
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
})();
