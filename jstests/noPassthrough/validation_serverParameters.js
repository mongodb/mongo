// Test collection validation related server parameter settings on server startup
// and via the setParameter command.

(function() {
'use strict';

load("jstests/noPassthrough/libs/server_parameter_helpers.js");

// Valid parameter values are in the range [0, infinity).
testNumericServerParameter('maxValidateMBperSec',
                           true /*isStartupParameter*/,
                           true /*isRuntimeParameter*/,
                           0 /*defaultValue*/,
                           60 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           -1 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);

// Valid parameter values are in the range (0, infinity).
testNumericServerParameter('maxValidateMemoryUsageMB',
                           true /*isStartupParameter*/,
                           true /*isRuntimeParameter*/,
                           200 /*defaultValue*/,
                           50 /*nonDefaultValidValue*/,
                           true /*hasLowerBound*/,
                           0 /*lowerOutOfBounds*/,
                           false /*hasUpperBound*/,
                           "unused" /*upperOutOfBounds*/);
})();
