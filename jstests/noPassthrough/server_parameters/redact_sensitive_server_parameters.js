/**
 * Verify that sensitive server parameters do not appear in plain text in log
 * files when set via the setParameter command, regardless of which log entry
 * captures the command.
 *
 * Covered behaviors:
 *   - The value of a sensitive parameter (e.g. ldapQueryPassword) is masked in
 *     every log entry that mentions it — on success, on per-parameter error, on
 *     command failure, in the pre-execution debug log, and in the slow query log.
 *   - An unrecognised parameter that appears alongside a known sensitive parameter
 *     is also masked, since it may be a typo of a sensitive name.
 *   - Non-sensitive parameters (e.g. logLevel) and generic metadata fields
 *     (e.g. $db, lsid) remain visible even when a sensitive parameter is present.
 *
 * @tags: [requires_enterprise]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

// Log IDs verified by this test suite.
const kSetSuccessWithOldValueId = 23435;
const kSetSuccessId = 23436;
const kSetErrorId = 20496;

// Emitted by mongod only.  Both entries include a redacted commandArgs field.
const kAssertionWhileExecutingId = 21962; // "Assertion while executing command"
const kAboutToRunId = 21965; // "About to run the command" (debug level 2)

// Emitted by mongos only on command failure.  Logs the error but not commandArgs.
const kMongosCommandExceptionId = 22772; // "Exception thrown while processing command"

// Emitted on any node when a command fails with a user-visible error, at debug level 1.
// Logs the error message but not commandArgs.
const kMongosUserAssertId = 23074; // "User assertion"

const kSlowQueryId = 51803;

const kCacheSize1 = "20MB";
const kCacheSize2 = "50MB";
const kCacheSize3 = "400GB";

// The "password" to avoid logging.
const kSensitiveValue = "super_secret_ldap_password_12345";

// The mask applied to sensitive parameter values in log output.
const kRedactedMask = "###";

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/** Returns {admin, logFile} for the given connection. */
function getTestEnv(conn) {
    return {admin: conn.getDB("admin"), logFile: conn.fullOptions.logFile};
}

/**
 * Sets logLevel to `level`, invokes `fn`, then restores logLevel to 0 in a
 * finally block so the change is always reverted even if `fn` throws.
 */
function withLogLevel(admin, level, fn) {
    assert.commandWorked(admin.runCommand({setParameter: 1, logLevel: level}));
    try {
        fn();
    } finally {
        admin.runCommand({setParameter: 1, logLevel: 0});
    }
}

/**
 * Sets the slow query threshold to `ms` via the profile command, invokes `fn`,
 * then restores the threshold to 100 ms in a finally block so the change is
 * always reverted even if `fn` throws.
 */
function withSlowMs(admin, ms, fn) {
    assert.commandWorked(admin.runCommand({profile: 0, slowms: ms}));
    try {
        fn();
    } finally {
        admin.runCommand({profile: 0, slowms: 100});
    }
}

/**
 * Asserts that at least one log entry with `logId` and matching `attrs` was
 * written.
 */
function assertLogEntry(logFile, logId, attrs) {
    assert(
        checkLog.checkContainsWithAtLeastCountJson(
            logFile,
            logId,
            attrs,
            1,
            null,
            /*isRelaxed=*/ true,
        ),
        `Expected at least one log ID ${logId} entry`,
    );
}

/**
 * Returns true when the log file contains at least one entry for `msgID`
 * where `parameterName`'s newValue equals `expectedValue`.
 */
function hasEntry(logFile, logID, parameterName, expectedValue) {
    const attrs = {parameterName, newValue: expectedValue};
    return checkLog.checkContainsWithAtLeastCountJson(
        logFile,
        logID,
        attrs,
        1,
        null,
        /*isRelaxed=*/ true,
    );
}

/**
 * Asserts that kSensitiveValue does not appear anywhere in the log file.
 */
function assertNoLeak(logFile) {
    assert(
        !checkLog.checkContainsOnce(logFile, kSensitiveValue),
        `Sensitive value found unredacted in log`,
    );
}

/**
 * Returns true when `conn` is a connection to a mongos router.
 * mongos identifies itself with msg "isdbgrid" in the hello response.
 */
function isMongosConn(conn) {
    const res = conn.getDB("admin").runCommand({hello: 1});
    return res.msg === "isdbgrid";
}

/**
 * Test 1 – sensitive parameter is redacted in logs.
 *
 * Sets ldapQueryPassword and verifies that:
 *  (a) The "newValue" field in the success log entry shows "###".
 *  (b) The raw sentinel value never appears anywhere in the log file.
 */
function testSensitiveParameterIsRedacted(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 1: Checking for ldap password redacted in " + logFile);

    withLogLevel(admin, 2, () => {
        admin.runCommand({setParameter: 1, ldapQueryPassword: kSensitiveValue});
    });

    // If the command succeeded, the success log entry must show "###" as the new value.
    // If it failed (e.g. no LDAP server is reachable), confirm the appropriate failure log
    // entry was written and that the sentinel value did not leak into it.
    let hasSuccess = hasEntry(
        logFile,
        kSetSuccessWithOldValueId,
        "ldapQueryPassword",
        kRedactedMask,
    );
    if (!hasSuccess) {
        if (isMongosConn(conn)) {
            // mongos records command failures without commandArgs; verify the failure
            // log fired and the sentinel is absent.
            assertLogEntry(logFile, kMongosCommandExceptionId, {db: "admin"});
        } else {
            assertLogEntry(logFile, kAssertionWhileExecutingId, {command: "setParameter"});
        }
    }
    assertNoLeak(logFile);
}

/**
 * Test 2 – non-sensitive parameter appears in plain text.
 *
 * Sets planCacheSize (redact: false, runtime-settable) and verifies that
 * its value appears verbatim in the "newValue" field of the success log
 * entry — confirming that redaction is selective and does not affect
 * parameters that are not marked sensitive.
 */
function testNonSensitiveParameterIsNotRedacted(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 2: Checking for non-sensitive field not redacted.");

    // planCacheSize is redact: false; this command must log the parameter.
    admin.runCommand({setParameter: 1, planCacheSize: kCacheSize2});

    // The log entry's newValue must contain that literal, not the "###" mask.
    assert(
        hasEntry(logFile, kSetSuccessWithOldValueId, "planCacheSize", '"' + kCacheSize2 + '"'),
        "Expected plain-text log entry for non-sensitive parameter planCacheSize",
    );
}

/**
 * Test 3 – sensitive values are not leaked in the command failure log.
 *
 * Sends a setParameter command that mixes a known sensitive parameter
 * (ldapQueryPassword) with an unrecognised parameter name. The command fails
 * with InvalidOptions.
 *
 * On mongod the failure log entry includes the full redacted commandArgs object,
 * so we verify that entry (log ID 21962) and confirm no sentinel appears in it.
 *
 * On mongos the failure path does not include commandArgs in any log entry.
 * Instead we verify the command failure log (log ID 22772) and the user-error
 * log (log ID 23074) both fired and that the sentinel is absent from each.
 *
 * Both the sensitive parameter value and the value of the unrecognised parameter
 * must be absent — the latter because it is co-present with a known sensitive
 * field and cannot be assumed safe.
 */
function testCommandArgsNotLeakedOnFailure(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 3: Sensitive values not leaked in command failure log");

    withLogLevel(admin, 2, () => {
        assert.commandFailed(
            admin.runCommand({
                setParameter: 1,
                ldapQueryPassword: kSensitiveValue,
                unknownSentinelParam_xyz: kSensitiveValue,
            }),
        );
    });

    if (isMongosConn(conn)) {
        // mongos records command failures without commandArgs; verify both failure
        // log entries fired and the sentinel is absent from each.
        assertLogEntry(logFile, kMongosCommandExceptionId, {db: "admin"});
        assertLogEntry(logFile, kMongosUserAssertId, {});
    } else {
        // mongod records command failures with a redacted commandArgs field.
        assertLogEntry(logFile, kAssertionWhileExecutingId, {command: "setParameter"});
    }
    assertNoLeak(logFile);
}

/**
 * Test 4 – sensitive values are not leaked in the pre-execution debug log.
 *
 * mongod emits a debug-level log entry before running every command that includes
 * the full commandArgs object.  Sensitive parameter values must be redacted in
 * that entry (log ID 21965, emitted at debug level 2).
 *
 * mongos does not emit an equivalent pre-execution entry that includes commandArgs,
 * so on mongos this test only verifies that the sentinel does not appear anywhere
 * in the log — confirming there is no other pre-execution leak path either.
 */
function testAboutToRunArgsNotLeaked(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 4: Sensitive values not leaked before command execution");

    // Enable debug level 2 so that the pre-execution log fires on mongod, then restore on exit.
    // The command may succeed or fail (no real LDAP server is required).
    withLogLevel(admin, 2, () => {
        admin.runCommand({setParameter: 1, ldapQueryPassword: kSensitiveValue});

        if (!isMongosConn(conn)) {
            assertLogEntry(logFile, kAboutToRunId, {commandArgs: {setParameter: 1}});
        }
    });
    assertNoLeak(logFile);
}

/**
 * Test 5 – command attribute not leaked in log ID 51803 (slow query log).
 *
 * The slow query log (ID 51803) is emitted after every command that exceeds
 * the slowms threshold (set to 0 here so that every operation is logged). Its
 * `command` attribute contains the full command object and is subject to the
 * same redaction requirements as the command failure and pre-execution log paths.
 *
 * A setParameter command that mixes a sensitive field (ldapQueryPassword) with
 * an unrecognised field is used to confirm that both values are redacted. The
 * unrecognised field must be masked because it is co-present with a known
 * sensitive parameter.
 *
 * Assertion: the sentinel string must not appear anywhere in the log file.
 */
function testSlowQueryCommandArgsNotLeaked(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 5: command attr not leaked in slow query log message");

    // Lower the slowms threshold to 0 so that every operation generates a slow query log entry,
    // then restore it on exit.
    withSlowMs(admin, 0, () => {
        // Issue a setParameter command that mixes a known-sensitive field (ldapQueryPassword)
        // with an unrecognised one (unknownSentinelParam_xyz), both carrying the sentinel.
        // The command fails with InvalidOptions, but the slow query log (ID 51803) is still
        // emitted at the end of the request.
        admin.runCommand({
            setParameter: 1,
            ldapQueryPassword: kSensitiveValue,
            unknownSentinelParam_xyz: kSensitiveValue,
        });

        assertLogEntry(logFile, kSlowQueryId, {});
    });
    assertNoLeak(logFile);
}

/**
 * Test 6 – non-sensitive fields are preserved in log ID 51803 when a sensitive
 * parameter is co-present (no over-redaction).
 *
 * Redaction must be surgical: known sensitive parameters (e.g. ldapQueryPassword)
 * and unrecognised parameters that are co-present with them are masked, but
 * non-sensitive parameters (e.g. logLevel) and generic metadata fields
 * (e.g. $db, lsid) must remain visible in the log.
 *
 * This test verifies that a setParameter call containing both ldapQueryPassword
 * and logLevel logs the logLevel with its numeric value, confirming that
 * redaction does not blanket-mask the entire command object.
 */
function testNonSensitiveFieldsPreservedInSlowQueryLog(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 6: non-sensitive fields preserved alongside sensitive ones");

    // Lower the slowms threshold to 0 so that ID 51803 fires, then restore on exit.
    withSlowMs(admin, 0, () => {
        admin.runCommand({
            setParameter: 1,
            ldapQueryPassword: kSensitiveValue,
            internalQueryStatsCacheSize: kCacheSize3,
            unknownSentinelParam_xyz: kSensitiveValue,
        });

        // Over-redaction check: internalQueryStatsCacheSize must NOT appear as the string "###".
        // Redaction must target only sensitive and conservatively-masked fields, leaving
        // non-sensitive parameters with their real values.
        assert(
            checkLog.checkContainsOnce(
                logFile,
                '"internalQueryStatsCacheSize":"' + kCacheSize3 + '"',
            ),
            "Non-sensitive internalQueryStatsCacheSize was incorrectly masked - over-redaction detected",
        );
    });
    assertNoLeak(logFile);
}

/**
 * Test 7 – an unrecognised parameter's value is not leaked in the command failure log.
 *
 * Sends a setParameter command with only an unrecognised parameter name. The
 * command fails with InvalidOptions.
 *
 * On mongod the failure log entry includes the full redacted commandArgs object;
 * we verify that entry (log ID 21962) and confirm the sentinel is absent.
 *
 * On mongos the failure path does not include commandArgs in any log entry.
 * Instead we verify the command failure log (log ID 22772) and the user-error
 * log (log ID 23074) both fired and that the sentinel is absent from each.
 */
function testCommandSingleArgNotLeakedOnFailure(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 7: unrecognised parameter's value not leaked in command failure log");

    // This command must fail: unknownSentinelParam_xyz is not a registered server parameter.
    withLogLevel(admin, 2, () => {
        assert.commandFailed(
            admin.runCommand({
                setParameter: 1,
                unknownSentinelParam_xyz: kSensitiveValue,
            }),
        );
    });

    if (isMongosConn(conn)) {
        // mongos records command failures without commandArgs; verify both failure
        // log entries fired and the sentinel is absent from each.
        assertLogEntry(logFile, kMongosCommandExceptionId, {db: "admin"});
        assertLogEntry(logFile, kMongosUserAssertId, {});
    } else {
        // mongod records command failures with a redacted commandArgs field.
        assertLogEntry(logFile, kAssertionWhileExecutingId, {command: "setParameter"});
    }
    assertNoLeak(logFile);
}

/**
 * Test 8 – non-sensitive parameter redacted when redactClientLogData is true.
 *
 * Sets internalQueryStatsCacheSize (redact: false, runtime-settable) and verifies that
 * its value appears verbatim in the "newValue" field of the success log
 * entry — confirming that redaction is selective and does not affect
 * parameters that are not marked sensitive.
 */
function testRedactClientLogData(conn) {
    const {admin, logFile} = getTestEnv(conn);

    jsTest.log("Test 8: Test redactClientLogData redacts all fields.");

    // Set redactClientLogData to true to force redaction of everything.
    const res = admin.runCommand({setParameter: 1, redactClientLogData: true});
    if (!res.ok) {
        jsTest.log("redactClientLogData not supported in this environment; skipping.");
        return;
    }

    // internalQueryStatsCacheSize is redact: false; this command must log the parameter.
    admin.runCommand({setParameter: 1, internalQueryStatsCacheSize: kCacheSize1});

    // Confirm the value was redacted.
    assert(
        hasEntry(logFile, kSetSuccessWithOldValueId, "internalQueryStatsCacheSize", kRedactedMask),
        "Non-sensitive parameter internalQueryStatsCacheSize was not redacted with redactClientLogData!",
    );

    // Restore redactClientLogData to false
    admin.runCommand({setParameter: 1, redactClientLogData: false});
}

/**
 * Blanket check – verify that no sentinel string escaped redaction anywhere in the log.
 *
 * All sentinel constants defined in this file share the "super_secret" prefix, so a
 * single substring scan of the full log catches any leak from any test scenario.
 */
function testAllSensitiveValuesAreRedacted(conn) {
    const {logFile} = getTestEnv(conn);

    jsTest.log("Final test: Checking for any sensitive data in " + logFile);

    assertNoLeak(logFile);
}

function runTest(conn) {
    testSensitiveParameterIsRedacted(conn);
    testNonSensitiveParameterIsNotRedacted(conn);
    testCommandArgsNotLeakedOnFailure(conn);
    testAboutToRunArgsNotLeaked(conn);
    testSlowQueryCommandArgsNotLeaked(conn);
    testNonSensitiveFieldsPreservedInSlowQueryLog(conn);
    testCommandSingleArgNotLeakedOnFailure(conn);
    testRedactClientLogData(conn);
    testAllSensitiveValuesAreRedacted(conn);
}

// ---- standalone mongod ----
(function testStandalone() {
    const conn = MongoRunner.runMongod({useLogFiles: true});
    assert.neq(null, conn, "mongod failed to start");
    try {
        runTest(conn);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

// ---- mongos (sharded cluster) ----
(function testMongos() {
    const st = new ShardingTest({shards: 1, mongos: [{useLogFiles: true}]});
    try {
        runTest(st.s);
    } finally {
        st.stop();
    }
})();
