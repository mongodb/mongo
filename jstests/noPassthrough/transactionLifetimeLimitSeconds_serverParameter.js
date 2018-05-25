// Test server validation of the 'transactionLifetimeLimitSeconds' server parameter setting on
// startup and via setParameter command. Valid parameter values are in the range [1, infinity).

(function() {
    'use strict';

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");

    // transactionLifetimeLimitSeconds is set to be higher than its default value in test suites.
    delete TestData.transactionLifetimeLimitSeconds;

    testNumericServerParameter("transactionLifetimeLimitSeconds",
                               true /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               60 /*defaultValue*/,
                               30 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);
})();
