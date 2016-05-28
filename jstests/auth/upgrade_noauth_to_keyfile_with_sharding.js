// Tests access control upgrade on a sharded cluster
// The purpose is to verify the connectivity between mongos, config server, and the shards

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    // Disable auth explicitly
    var noAuthOptions = {noauth: ''};
    var transitionToAuthOptions = {
        clusterAuthMode: 'keyFile',
        keyFile: KEYFILE,
        transitionToAuth: ''
    };
    var keyFileOptions = {clusterAuthMode: 'keyFile', keyFile: KEYFILE};

    print('=== Testing no-auth/transitionToAuth cluster ===');
    mixedShardTest(noAuthOptions, transitionToAuthOptions, true);
    mixedShardTest(transitionToAuthOptions, noAuthOptions, true);

    print('=== Testing transitionToAuth/transitionToAuth cluster ===');
    mixedShardTest(transitionToAuthOptions, transitionToAuthOptions, true);

    print('=== Testing transitionToAuth/keyFile cluster ===');
    mixedShardTest(keyFileOptions, transitionToAuthOptions, true);
    mixedShardTest(transitionToAuthOptions, keyFileOptions, true);

    print('=== Testing no-auth/keyFile cluster fails ===');
    mixedShardTest(noAuthOptions, keyFileOptions, false);
    mixedShardTest(keyFileOptions, noAuthOptions, false);
}());
