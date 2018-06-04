// Tests the ttlMonitorSleepSecs parameter

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/server_parameter_helpers.js');

    testNumericServerParameter('ttlMonitorSleepSecs',
                               true,     // is Startup Param
                               false,    // is runtime param
                               60,       // default value
                               30,       // valid, non-default value
                               true,     // has lower bound
                               0,        // out of bound value (below lower bound)
                               false,    // has upper bound
                               'unused'  // out of bounds value (above upper bound)
                               );
})();
