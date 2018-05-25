// Tests the maxTransactionLockRequestTimeoutMillis server parameter.

(function() {
    'use strict';

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");

    // Valid parameter values are in the range (-infinity, infinity).
    testNumericServerParameter("maxTransactionLockRequestTimeoutMillis",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               0 /*defaultValue*/,
                               30 /*nonDefaultValidValue*/,
                               false /*hasLowerBound*/,
                               "unused" /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);
})();
