/**
 * Verifies that the validate hook is able to upgrade the feature compatibility version of the
 * server regardless of what state any previous upgrades or downgrades have left it in (besides
 * the isCleaningServerMetadata state, where we must complete the transition before upgrading).
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// We skip doing the data consistency checks while terminating the cluster because they conflict
// with the counts of the number of times the "validate" command is run.
TestData.skipCollectionAndIndexValidation = true;

function makePatternForValidate(dbName, collName) {
    return new RegExp(
        `Slow query.*"ns":"${dbName}\\.\\$cmd".*"appName":"MongoDB Shell","command":{"validate":"${collName}"`,
        "g",
    );
}

function makePatternForSetFCV(targetVersion) {
    return new RegExp(
        `Slow query.*"appName":"MongoDB Shell","command":{"setFeatureCompatibilityVersion":"${targetVersion}"`,
        "g",
    );
}

function makePatternForSetParameter(paramName) {
    return new RegExp(`Slow query.*"appName":"MongoDB Shell","command":{"setParameter":1,"${paramName}":`, "g");
}

function countMatches(pattern, output) {
    assert(pattern.global, "the 'g' flag must be used to find all matches");

    let numMatches = 0;
    while (pattern.exec(output) !== null) {
        ++numMatches;
    }
    return numMatches;
}

function runValidateHook(testCase) {
    clearRawMongoProgramOutput();

    // NOTE: once modules are imported they are cached, so we need to run this in a parallel shell.
    const awaitShell = startParallelShell(async function () {
        globalThis.db = db.getSiblingDB("test");
        TestData.forceValidationWithFeatureCompatibilityVersion = latestFCV;
        await import("jstests/hooks/run_validate_collections.js");
    }, testCase.conn.port);

    awaitShell();

    // We terminate the processes to ensure that the next call to rawMongoProgramOutput()
    // will return all of their output.
    testCase.teardown();

    return rawMongoProgramOutput(".*");
}

function testStandalone(
    additionalSetupFn,
    {
        expectedAtTeardownFCV,
        expectedSetLastLTSFCV: expectedSetLastLTSFCV = 0,
        expectedSetLatestFCV: expectedSetLatestFCV = 0,
    } = {},
) {
    const conn = MongoRunner.runMongod({setParameter: {logComponentVerbosity: tojson({command: 1})}});
    assert.neq(conn, "mongod was unable to start up");

    // Insert a document so the "validate" command has some actual work to do.
    assert.commandWorked(conn.getDB("test").mycoll.insert({}));

    // Run the additional setup function to put the server into the desired state.
    additionalSetupFn(conn);

    const output = runValidateHook({
        conn: conn,
        teardown: () => {
            // The validate hook should leave the server with a feature compatibility version of
            // 'expectedAtTeardownFCV' and no targetVersion.
            checkFCV(conn.getDB("admin"), expectedAtTeardownFCV);
            MongoRunner.stopMongod(conn);
        },
    });

    let pattern = makePatternForValidate("test", "mycoll");
    assert.eq(
        1,
        countMatches(pattern, output),
        "expected to find " + tojson(pattern) + " from mongod in the log output",
    );

    for (let [targetVersion, expectedCount] of [
        [lastLTSFCV, expectedSetLastLTSFCV],
        [latestFCV, expectedSetLatestFCV],
    ]) {
        // Since the additionalSetupFn() function may run the setFeatureCompatibilityVersion
        // command and we don't have a guarantee those log messages were cleared when
        // clearRawMongoProgramOutput() was called, we assert 'expectedSetLastLTSFCV' and
        // 'expectedSetLatestFCV' as lower bounds.
        const pattern = makePatternForSetFCV(targetVersion);
        assert.lte(
            expectedCount,
            countMatches(pattern, output),
            "expected to find " + tojson(pattern) + " from mongod in the log output",
        );
    }

    pattern = makePatternForSetParameter("transactionLifetimeLimitSeconds");
    assert.eq(
        2,
        countMatches(pattern, output),
        "expected to find " + tojson(pattern) + " from mongod in the log output twice",
    );
}

function forceInterruptedUpgradeOrDowngrade(conn, targetVersion) {
    // We create a separate connection to the server exclusively for running the
    // setFeatureCompatibilityVersion command so only that operation is ever interrupted by
    // the checkForInterruptFail failpoint.
    const setFCVConn = new Mongo(conn.host);
    const myUriRes = assert.commandWorked(setFCVConn.adminCommand({whatsmyuri: 1}));
    const myUri = myUriRes.you;

    const curOpRes = assert.commandWorked(setFCVConn.adminCommand({currentOp: 1, client: myUri}));
    const threadName = curOpRes.inprog[0].desc;

    assert.commandWorked(
        conn.adminCommand({
            configureFailPoint: "checkForInterruptFail",
            mode: "alwaysOn",
            data: {threadName, chance: 0.05},
        }),
    );

    let attempts = 0;
    assert.soon(function () {
        let res = setFCVConn.adminCommand({setFeatureCompatibilityVersion: targetVersion, confirm: true});

        if (res.ok === 1) {
            assert.commandWorked(res);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
        }

        ++attempts;

        res = assert.commandWorked(conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));

        if (res.featureCompatibilityVersion.hasOwnProperty("targetVersion")) {
            const fcvDoc = conn
                .getDB("admin")
                .system.version.find({_id: "featureCompatibilityVersion"})
                .limit(1)
                .readConcern("local")
                .next();
            if (fcvDoc.isCleaningServerMetadata) {
                checkFCV(conn.getDB("admin"), lastLTSFCV, targetVersion, true /*isCleaningServerMetadata*/);

                // If the setFCV command was interrupted during the isCleaningServerMetadata
                // state, complete the FCV transition successfully (downgrade or upgrade).
                assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: targetVersion, confirm: true}));
            } else {
                checkFCV(conn.getDB("admin"), lastLTSFCV, targetVersion);
                jsTest.log(`Reached partial transition to ${targetVersion} after ${attempts} attempts`);
                return true;
            }
        }

        // Either upgrade the feature compatibility version so we can try downgrading again,
        // or downgrade the feature compatibility version so we can try upgrading again.
        // Note that we're using 'conn' rather than 'setFCVConn' to avoid the upgrade being
        // interrupted.
        assert.commandWorked(
            conn.adminCommand({
                setFeatureCompatibilityVersion: targetVersion === lastLTSFCV ? latestFCV : lastLTSFCV,
                confirm: true,
            }),
        );
    }, "failed to get featureCompatibilityVersion document into a partially downgraded" + " state");

    assert.commandWorked(
        conn.adminCommand({
            configureFailPoint: "checkForInterruptFail",
            mode: "off",
        }),
    );
}

// testStandaloneInLatestFCV
testStandalone((conn) => checkFCV(conn.getDB("admin"), latestFCV), {expectedAtTeardownFCV: latestFCV});

// testStandaloneInLastLTSFCV
testStandalone(
    (conn) => {
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        checkFCV(conn.getDB("admin"), lastLTSFCV);
    },
    {expectedAtTeardownFCV: lastLTSFCV, expectedSetLastLTSFCV: 1, expectedSetLatestFCV: 1},
);

// testStandaloneWithInterruptedFCVDowngrade
testStandalone(
    (conn) => {
        forceInterruptedUpgradeOrDowngrade(conn, lastLTSFCV);
    },
    {expectedAtTeardownFCV: lastLTSFCV, expectedSetLastLTSFCV: 2, expectedSetLatestFCV: 1},
);

// testStandaloneWithInterruptedFCVUpgrade
testStandalone(
    (conn) => {
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        forceInterruptedUpgradeOrDowngrade(conn, latestFCV);
    },
    {expectedAtTeardownFCV: lastLTSFCV, expectedSetLastLTSFCV: 1, expectedSetLatestFCV: 1},
);
