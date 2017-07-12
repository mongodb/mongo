// FTDC test cases
//
load('jstests/libs/ftdc.js');

(function() {
    'use strict';
    var admin = db.getSiblingDB("admin");

    verifyCommonFTDCParameters(admin, true);
})();
