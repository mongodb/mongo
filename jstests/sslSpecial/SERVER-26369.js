// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    load("jstests/ssl/libs/ssl_helpers.js");

    var st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    st.rs0.restart(0, {
        sslMode: "allowSSL",
        sslPEMKeyFile: "jstests/libs/password_protected.pem",
        sslPEMKeyPassword: "qwerty",
        sslCAFile: "jstests/libs/ca.pem",
        shardsvr: ''
    });

    st.stop();
})();
