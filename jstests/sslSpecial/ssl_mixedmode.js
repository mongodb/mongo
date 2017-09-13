// Test the --sslMode parameter
// This tests runs through the 8 possible combinations of sslMode values
// and SSL-enabled and disabled shell respectively. For each combination
// expected behavior is verified.

load("jstests/libs/ssl_test.js");

function testCombination(sslMode, sslShell, shouldSucceed) {
    var serverOptionOverrides = {sslMode: sslMode};

    var clientOptions =
        sslShell ? SSLTest.prototype.defaultSSLClientOptions : SSLTest.prototype.noSSLClientOptions;

    var fixture = new SSLTest(serverOptionOverrides, clientOptions);

    print("Trying sslMode: '" + sslMode + "' with sslShell = " + sslShell +
          "; expect connection to " + (shouldSucceed ? "SUCCEED" : "FAIL"));

    assert.eq(shouldSucceed, fixture.connectWorked());
}

testCombination("disabled", false, true);
testCombination("allowSSL", false, true);
testCombination("preferSSL", false, true);
testCombination("requireSSL", false, false);
testCombination("disabled", true, false);
testCombination("allowSSL", true, true);
testCombination("preferSSL", true, true);
testCombination("requireSSL", true, true);
