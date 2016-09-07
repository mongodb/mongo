/**
 * Test that verifies the javascript integration can handle large string exception messages.
 */
(function() {
    'use strict';

    var len = 65 * 1024 * 1024;
    var str = new Array(len + 1).join('b');

    // We expect to successfully throw and catch this large exception message.
    // We do not want the mongo shell to terminate.
    assert.throws(function() {
        throw str;
    });

})();
