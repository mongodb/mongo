/*
 * Tests simultaneous upgrade from noauth/no-ssl to x509/requireSSL on a sharded cluster.
 * The purpose is to verify the connectivity between mongos, config server, and the shards
 *
 * NOTE: This test is similar to the mixed_mode_sharded_transition.js in the sslSpecial
 * test suite. This suite must use ssl so it cannot test modes without ssl.
 */

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    var transitionToX509AllowSSL =
        Object.merge(allowSSL, {transitionToAuth: '', clusterAuthMode: 'x509'});
    var transitionToX509PreferSSL =
        Object.merge(preferSSL, {transitionToAuth: '', clusterAuthMode: 'x509'});
    var x509RequireSSL = Object.merge(requireSSL, {clusterAuthMode: 'x509'});

    function testCombos(opt1, opt2, shouldSucceed) {
        mixedShardTest(opt1, opt2, shouldSucceed);
        mixedShardTest(opt2, opt1, shouldSucceed);
    }

    print('=== Testing transitionToAuth/allowSSL - transitionToAuth/preferSSL cluster ===');
    testCombos(transitionToX509AllowSSL, transitionToX509PreferSSL, true);

    print('=== Testing transitionToAuth/preferSSL - transitionToAuth/preferSSL cluster ===');
    mixedShardTest(transitionToX509PreferSSL, transitionToX509PreferSSL, true);

    print('=== Testing transitionToAuth/preferSSL - x509/requireSSL cluster ===');
    testCombos(transitionToX509PreferSSL, x509RequireSSL, true);
}());
