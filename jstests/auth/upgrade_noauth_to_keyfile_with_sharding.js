// Tests access control upgrade on a sharded cluster
// The purpose is to verify the connectivity between mongos, config server, and the shards

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    // Disable auth explicitly
    var noAuthOptions = {
        noauth: ''
    };
    var tryClusterAuthOptions = {
        clusterAuthMode: 'keyFile',
        keyFile: KEYFILE,
        tryClusterAuth: ''
    };
    var keyFileOptions = {
        clusterAuthMode: 'keyFile',
        keyFile: KEYFILE
    };

    print('=== Testing no-auth/tryClusterAuth cluster ===');
    mixedShardTest(noAuthOptions, tryClusterAuthOptions, true);
    mixedShardTest(tryClusterAuthOptions, noAuthOptions, true);

    print('=== Testing tryClusterAuth/tryClusterAuth cluster ===');
    mixedShardTest(tryClusterAuthOptions, tryClusterAuthOptions, true);

    print('=== Testing tryClusterAuth/keyFile cluster ===');
    mixedShardTest(keyFileOptions, tryClusterAuthOptions, true);
    mixedShardTest(tryClusterAuthOptions, keyFileOptions, true);

    print('=== Testing no-auth/keyFile cluster fails ===');
    mixedShardTest(noAuthOptions, keyFileOptions, false);
    mixedShardTest(keyFileOptions, noAuthOptions, false);
}());
