/**
 * Tests defanging of invariants to ensure that the server parameter and source locations
 * are properly parsed and make sure the defanged invariant has tassert like behavior.
 *
 * The tests are ordered to minimize the number of server startup and shutdown cycles by
 * reusing each server for as many assertions as possible. Servers are only ever shut
 * down because the test asserted the server aborts, or because there are no assertions
 * left that can run against that particular server.
 *
 * @tags: [
 *   incompatible_with_windows_tls,
 *   incompatible_aubsan,
 * ]
 */

TestData.cleanUpCoreDumpsFromExpectedCrash = true;

const kFalseInvariantLocation = "src/mongo/util/false_invariant.inl:1";
const kDummyInvariantLocation = "path/dummy_invariant.cpp:12";

// Asserts that running {failInvariant: 1} aborts the server with the expected invariant
// failure logs. Must only be called on a server whose false invariant is not defanged,
// and must be the last assertion run against that server.
function assertFailInvariantAborts(conn) {
    clearRawMongoProgramOutput();

    assert.throws(() => conn.getDB("admin").runCommand({failInvariant: 1}));

    const exitCode = waitProgram(conn.pid);
    assert.eq(
        exitCode,
        MongoRunner.EXIT_ABORT,
        `Expected server to exit with signal 6 (SIGABRT), got ${exitCode}`,
    );

    const logContents = rawMongoProgramOutput(".*");
    assert(
        logContents.search(/"id":23079/) >= 0,
        "Expected 'Invariant failure' log message with ID 23079 in server output",
    );
    assert(
        logContents.search(/"id":23080/) >= 0,
        "Expected 'aborting after invariant() failure' log message with ID 23080 in server output",
    );

    MongoRunner.stopMongod(conn);
}

// Asserts that running {failInvariant: 1} on a server whose false invariant is defanged
// only yields an InternalError and leaves the server running.
function assertFailInvariantDefanged(conn) {
    assert.commandFailedWithCode(
        conn.getDB("admin").runCommand({failInvariant: 1}),
        ErrorCodes.InternalError,
    );
}

// Asserts that getParameter returns the whole 'defangedInvariants' value that was set.
function assertGetParameterReturnsValue(conn, expected) {
    const res = assert.commandWorked(conn.adminCommand({getParameter: 1, defangedInvariants: 1}));
    assert.docEq(
        expected,
        res.defangedInvariants,
        "getParameter should return the whole 'defangedInvariants' value that was set",
    );
}

function setServerParameterRuntime(conn, defangedInvariants) {
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, defangedInvariants: defangedInvariants}),
    );
}

// Case 1: a server with no defanged invariants. Used to verify that the false invariant
// aborts the server when it is not defanged, and to determine the build version needed to
// construct 'defangedInvariants' values for the later servers.
const conn = MongoRunner.runMongod({});
const version = assert.commandWorked(conn.adminCommand({buildInfo: 1})).version;
assertFailInvariantAborts(conn);

// Case 2: a server started without any defanged invariants, reused for all assertions
// that only need the parameter to be set at runtime. Ends by verifying that replacing the
// parameter value with one that no longer defangs the false invariant makes the invariant
// abort the server again.
{
    const conn = MongoRunner.runMongod({});

    // The value associated with the current version must be an array of invariant
    // locations.
    assert.commandFailedWithCode(
        conn.adminCommand({
            setParameter: 1,
            defangedInvariants: {[version]: kFalseInvariantLocation},
        }),
        ErrorCodes.TypeMismatch,
        "Expected the parameter to reject a non-array value for the current version",
    );

    // Every element of the array must be a string.
    assert.commandFailedWithCode(
        conn.adminCommand({
            setParameter: 1,
            defangedInvariants: {[version]: [kFalseInvariantLocation, 42]},
        }),
        ErrorCodes.TypeMismatch,
        "Expected the parameter to reject an array containing a non-string element",
    );

    const defangCurrentVersionOnly = {[version]: [kFalseInvariantLocation]};
    setServerParameterRuntime(conn, defangCurrentVersionOnly);
    assertGetParameterReturnsValue(conn, defangCurrentVersionOnly);

    const defangWithExtraVersion = {
        ...defangCurrentVersionOnly,
        "0.0.0-test": [kDummyInvariantLocation],
    };
    setServerParameterRuntime(conn, defangWithExtraVersion);
    assertGetParameterReturnsValue(conn, defangWithExtraVersion);

    // A runtime-defanged invariant must not abort the server.
    assertFailInvariantDefanged(conn);

    // Replacing the value with one that no longer defangs the false invariant must make
    // the invariant abort the server again.
    setServerParameterRuntime(conn, {[version]: [kDummyInvariantLocation]});
    assertFailInvariantAborts(conn);
}

// Case 3: a runtime-defanged server, used to verify that replacing the parameter value
// with one that has no entry for the current version makes the invariant abort the server
// again. Must be the last assertion run against this server.
{
    const conn = MongoRunner.runMongod({});
    setServerParameterRuntime(conn, {[version]: [kFalseInvariantLocation]});
    setServerParameterRuntime(conn, {"0.0.0-test": [kFalseInvariantLocation]});

    assertFailInvariantAborts(conn);
}

// Case 4: a server defanged at startup with only the current version, used to verify
// that getParameter returns the whole value set at startup and that a startup-defanged
// invariant does not abort the server.
{
    const defangCurrentVersionOnly = {[version]: [kFalseInvariantLocation]};
    const conn = MongoRunner.runMongod({
        setParameter: "defangedInvariants=" + tojson(defangCurrentVersionOnly),
    });
    assertGetParameterReturnsValue(conn, defangCurrentVersionOnly);

    assertFailInvariantDefanged(conn);

    MongoRunner.stopMongod(conn, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

// Case 5: a server defanged at startup with an entry for an additional version, used to
// verify that getParameter returns the whole value set at startup.
{
    const defangWithExtraVersion = {
        [version]: [kFalseInvariantLocation],
        "0.0.0-test": [kDummyInvariantLocation],
    };
    const conn = MongoRunner.runMongod({
        setParameter: "defangedInvariants=" + tojson(defangWithExtraVersion),
    });
    assertGetParameterReturnsValue(conn, defangWithExtraVersion);

    MongoRunner.stopMongod(conn);
}
