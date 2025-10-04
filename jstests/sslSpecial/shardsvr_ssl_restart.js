// Test restarts a single repl set node and it will never have repl config when it restarts
// so it won't be able to transition to primary.
// @tags: [requires_persistence]

// Validate the shardsrvr does not crash when enabling SSL with encrypted PEM for a cluster
// Checking UUID consistency involves talking to a shard node, which in this test is shutdown
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: {rs0: {nodes: 1}}});
let opts = {
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: "jstests/libs/client.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    shardsvr: "",
};
requireSSLProvider("openssl", function () {
    // Only the OpenSSL provider supports encrypted PKCS#8
    opts.tlsCertificateKeyFile = "jstests/libs/password_protected.pem";
    opts.tlsCertificateKeyFilePassword = "qwerty";
});

st.rs0.restart(0, opts);
st.stop();
