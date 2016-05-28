/*
 * Tests simultaneous upgrade from noauth/no-ssl to x509/requireSSL on a sharded cluster.
 * The purpose is to verify the connectivity between mongos, config server, and the shards
 *
 * NOTE: This test is similar to the mixed_mode_sharded_transition.js in the ssl
 * test suite. This suite does not use ssl so it cannot test modes with ssl.
 */

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    // Disable auth explicitly
    var noAuthOptions = {noauth: ''};
    var transitionToX509AllowSSL =
        Object.merge(allowSSL, {transitionToAuth: '', clusterAuthMode: 'x509'});
    var x509RequireSSL = Object.merge(requireSSL, {clusterAuthMode: 'x509'});

    print('=== Testing no-auth/transitionToAuth cluster ===');
    mixedShardTest(noAuthOptions, transitionToX509AllowSSL, true);
    mixedShardTest(transitionToX509AllowSSL, noAuthOptions, true);

    print('=== Testing transitionToAuth/transitionToAuth cluster ===');
    mixedShardTest(transitionToX509AllowSSL, transitionToX509AllowSSL, true);
}());
