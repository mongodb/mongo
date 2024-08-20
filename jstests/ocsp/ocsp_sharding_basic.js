// Check that OCSP verification works
// @tags: [requires_http_client]

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FAULT_REVOKED, MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {clearOCSPCache, OCSP_CA_CERT, OCSP_SERVER_CERT} from "jstests/ocsp/lib/ocsp_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() === "apple") {
    quit();
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

jsTest.log("Test a ShardingTest without MockOCSPServer.");
test();

let mock_ocsp = new MockOCSPServer("", 10000);
mock_ocsp.start();

clearOCSPCache();

jsTest.log("Test a ShardingTest with MockOCSPServer and expect to have valid OCSP response.");
test();

// We don't want to invoke the hang analyzer because we
// expect this test to fail by timing out
MongoRunner.runHangAnalyzer.disable();

clearOCSPCache();

// Leave the OCSP responder on so that the other nodes all have valid responses.
jsTest.log("Test another ShardingTest with MockOCSPServer and expect to have valid OCSP response.");
var st = new ShardingTest(sharding_config);

mock_ocsp.stop();
mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

clearOCSPCache();
sleep(2000);

jsTest.log("Restart the mongos with MockOCSPServer and expect to have REVOKED response.");
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
jsTest.log("Restart the mongos with MockOCSPServer and expect to have valid OCSP response.");
st.restartMongos(0);

mock_ocsp.stop();
st.stop();
