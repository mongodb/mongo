// Test the --tlsMode parameter
// This tests runs through the 8 possible combinations of tlsMode values
// and SSL-enabled and disabled shell respectively. For each combination
// expected behavior is verified.
import {TLSTest} from "jstests/libs/ssl_test.js";
function testCombination(tlsMode, sslShell, shouldSucceed) {
    jsTest.log("TESTING: tlsMode = " + tlsMode + ", sslShell = " +
               (sslShell ? "true"
                         : "false" +
                        " (should " + (shouldSucceed ? "" : "not ") + "succeed)"));

    var serverOptionOverrides = {tlsMode: tlsMode, setParameter: {enableTestCommands: 1}};

    var clientOptions =
        sslShell ? TLSTest.prototype.defaultTLSClientOptions : TLSTest.prototype.noTLSClientOptions;

    var fixture = new TLSTest(serverOptionOverrides, clientOptions);

    if (shouldSucceed) {
        assert(fixture.connectWorked());
    } else {
        assert(fixture.connectFails());
    }
}

testCombination("disabled", false, true);
testCombination("allowTLS", false, true);
testCombination("preferTLS", false, true);
testCombination("requireTLS", false, false);
testCombination("disabled", true, false);
testCombination("allowTLS", true, true);
testCombination("preferTLS", true, true);
testCombination("requireTLS", true, true);
