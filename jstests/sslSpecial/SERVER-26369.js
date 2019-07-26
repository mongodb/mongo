// Validate the shardsrvr does not crash when enabling SSL with encrypted PEM for a cluster
// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
'use strict';

load("jstests/ssl/libs/ssl_helpers.js");

const st = new ShardingTest({shards: {rs0: {nodes: 1}}});
let opts = {
    sslMode: "allowSSL",
    sslPEMKeyFile: "jstests/libs/client.pem",
    sslCAFile: "jstests/libs/ca.pem",
    shardsvr: ''
};
requireSSLProvider('openssl', function() {
    // Only the OpenSSL provider supports encrypted PKCS#8
    opts.sslPEMKeyFile = "jstests/libs/password_protected.pem";
    opts.sslPEMKeyPassword = "qwerty";
});

st.rs0.restart(0, opts);
st.stop();
})();
