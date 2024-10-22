/**
 * Tests various failure cases when using certificate selectors on Windows.
 */
import {
    requireSSLProvider,
    TRUSTED_SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

const notFoundError = "failed to find cert";
const badValueError = "Invalid certificate selector value";
const startupFailureTestCases = [
    {selector: `thumbprint=DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEF`, error: notFoundError},
    {selector: `subject=Unknown Test Client`, error: notFoundError},
    {selector: `thumbprint=LOL`, error: badValueError},
    {
        keyFile: TRUSTED_SERVER_CERT,
        clusterSelector: `thumbprint=DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEF`,
        error: notFoundError
    },
    {
        keyFile: TRUSTED_SERVER_CERT,
        clusterSelector: `subject=Unknown Test Client`,
        error: notFoundError
    },
    {keyFile: TRUSTED_SERVER_CERT, clusterSelector: `thumbprint=LOL`, error: badValueError},
];

function testStartupFails(testCase) {
    jsTestLog(`Running testStartupFails with test case: ${tojson(testCase)}`);
    const opts = {
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: testCase.keyFile,
        tlsCertificateSelector: testCase.selector,
        tlsClusterCertificateSelector: testCase.clusterSelector,
        tlsAllowInvalidHostnames: "",
        setParameter: {tlsUseSystemCA: true},
        waitForConnect: true,
    };
    clearRawMongoProgramOutput();
    assert.throws(() => {
        MongoRunner.runMongod(opts);
    });
    assert(rawMongoProgramOutput(".*").includes(testCase.error));
}

requireSSLProvider('windows', function() {
    startupFailureTestCases.forEach(test => testStartupFails(test));
});
