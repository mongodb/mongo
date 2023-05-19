// Check that OCSP verification works
// @tags: [requires_http_client]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

if (determineSSLProvider() === "apple") {
    return;
}

const ocsp_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_CERT,
    sslCAFile: OCSP_CA_CERT,
    sslAllowInvalidHostnames: "",
};

const sharding_config = {
    shards: 1,
    mongos: 1,
    other: {
        configOptions: ocsp_options,
        mongosOptions: ocsp_options,
        rsOptions: ocsp_options,
        shardOptions: ocsp_options,
    }
};

function test() {
    assert.doesNotThrow(() => {
        let st = new ShardingTest(sharding_config);

        st.getConnNames();
        st.stop();
    });
}

clearOCSPCache();

test();

let mock_ocsp = new MockOCSPServer("", 10000);
mock_ocsp.start();

clearOCSPCache();

test();

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

clearOCSPCache();

// Leave the OCSP responder on so that the other nodes all have valid responses.
var st = new ShardingTest(sharding_config);

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

clearOCSPCache();
sleep(2000);

const err = assert.throws(() => {
    st.restartMongos(0);
});

mock_ocsp.stop();

const errMsg = err.toString();

assert.gte(errMsg.search("assert.soon failed"), 0, "Test failed for wrong reason: " + err);

sleep(2000);

MongoRunner.runHangAnalyzer.enable();

mock_ocsp = new MockOCSPServer("", 10000);
mock_ocsp.start();

// Get the mongos back up again so that we can shutdown the ShardingTest.
st.restartMongos(0);

mock_ocsp.stop();
st.stop();
}());
