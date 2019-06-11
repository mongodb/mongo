/**
 * Test server validation of the 'wiredTigerMaxCacheOverflowSizeGB' server parameter setting via
 * the setParameter command.
 * @tags: [requires_persistence, requires_wiredtiger]
 */

(function() {
    'use strict';

    load("jstests/noPassthrough/libs/server_parameter_helpers.js");

    // Valid parameter values are in the range [0.1, infinity) or 0 (unbounded).
    testNumericServerParameter("wiredTigerMaxCacheOverflowSizeGB",
                               false /*isStartupParameter*/,
                               true /*isRuntimeParameter*/,
                               0 /*defaultValue*/,
                               0.1 /*nonDefaultValidValue*/,
                               true /*hasLowerBound*/,
                               0.09 /*lowerOutOfBounds*/,
                               false /*hasUpperBound*/,
                               "unused" /*upperOutOfBounds*/);
})();
