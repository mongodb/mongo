/**
 * Test for startup warning when X509 auth and tlsAllowInvalidCertificates are enabled.
 * TODO SERVER-99909 Re-enable this test under TSAN once shutdown-during-startup issues are fixed.
 * @tags: [
 *   tsan_incompatible,
 * ]
 */

// Due to a race condition in shutdown-during-startup, this test sometimes produces core dumps.
// TODO SERVER-99909 Remove this once shutdown-during-startup issues are fixed.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

function runTest(checkMongos, opts, expectWarningCertifcates, expectWarningHostnames) {
    clearRawMongoProgramOutput();
    let mongo;

    if (checkMongos) {
        mongo = MongoRunner.runMongos(
            Object.assign(
                {
                    configdb: "fakeRS/localhost:27017",
                    waitForConnect: false,
                },
                opts,
            ),
        );
    } else {
        mongo = MongoRunner.runMongod(
            Object.assign(
                {
                    auth: "",
                    tlsMode: "preferTLS",
                    tlsCertificateKeyFile: "jstests/libs/server.pem",
                    tlsCAFile: "jstests/libs/ca.pem",
                    waitForConnect: false,
                },
                opts,
            ),
        );
    }

    assert.soon(function () {
        const output = rawMongoProgramOutput(".*");
        return (
            expectWarningCertifcates ==
                output.includes(
                    "While invalid X509 certificates may be used to connect to this server, they will not be considered permissible for authentication",
                ) &&
            expectWarningHostnames ==
                output.includes(
                    "This server will not perform X.509 hostname validation. This may allow your server to make or accept connections to untrusted parties",
                )
        );
    });

    stopMongoProgramByPid(mongo.pid);
}

function runTests(checkMongos) {
    // Don't expect a warning for certificates and hostnames when we're not using both options
    // together.
    runTest(checkMongos, {}, false, false);

    // Do expect a warning for certificates when we're combining options.
    runTest(checkMongos, {tlsAllowInvalidCertificates: ""}, true, false);

    // Do expect a warning for hostnames.
    runTest(checkMongos, {tlsAllowInvalidHostnames: ""}, false, true);

    // Do expect a warning for certificates and hostnames.
    runTest(checkMongos, {tlsAllowInvalidCertificates: "", tlsAllowInvalidHostnames: ""}, true, true);
}

// Run tests on mongos
runTests(true);

// Run tests on mongod
runTests(false);
