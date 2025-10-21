/**
 * Tests for the ingress request rate limiter application-based exemptions.
 * @tags: [requires_fcv_82]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    authenticateConnection,
    getRateLimiterStats,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const testGetSetAppExemptions = () => {
    const kExemptions = ["foo", "bar", "baz"];
    const standalone = MongoRunner.runMongod({
        setParameter: {
            ingressRequestRateLimiterApplicationExemptions: {appNames: kExemptions},
        },
    });

    // run getParameter to verify the exemptions were set correctly.
    const getParamRes = assert.commandWorked(
        standalone.adminCommand(
            {getParameter: 1, ingressRequestRateLimiterApplicationExemptions: 1}),
    );
    assert.eq(getParamRes.ingressRequestRateLimiterApplicationExemptions, kExemptions);

    // run getParameter with new exemptions to verify the exemptions can be updated correctly.
    const newExemptions = ["newFoo", "newBar"];
    assert.commandWorked(
        standalone.adminCommand({
            setParameter: 1,
            ingressRequestRateLimiterApplicationExemptions: {appNames: newExemptions},
        }),
    );

    const getParamRes2 = assert.commandWorked(
        standalone.adminCommand(
            {getParameter: 1, ingressRequestRateLimiterApplicationExemptions: 1}),
    );
    assert.eq(getParamRes2.ingressRequestRateLimiterApplicationExemptions, newExemptions);

    MongoRunner.stopMongod(standalone);
};

const testAppExemptionsWorkInReplSet = () => {
    const replSet = new ReplSetTest({
        nodes: 3,
        keyFile: "jstests/libs/key1",
        nodeOptions: {
            auth: "",
            setParameter: {
                logComponentVerbosity: tojson({command: 2}),
                "failpoint.ingressRequestRateLimiterFractionalRateOverride": tojson({
                    mode: "alwaysOn",
                    data: {rate: kSlowestRefreshRateSecs},
                }),
                ingressRequestAdmissionRatePerSec: 1,
                ingressRequestAdmissionBurstCapacitySecs: Math.round(1.0 / kSlowestRefreshRateSecs),
                // At first, lets just exempt the replication clients and an appName for the shell
                // connection.
                ingressRequestRateLimiterApplicationExemptions: {
                    appNames:
                        [kRateLimiterExemptAppName, "OplogFetcher", "NetworkInterfaceTL-Repl"],
                },
                ingressRequestRateLimiterEnabled: false,  // kept disabled during repl set setup
            },
        },
    });

    // We setup the replset safely with rate limiting disabled.
    replSet.startSet();
    replSet.initiate();

    replSet.awaitSecondaryNodes();

    const kUser = "admin";
    const kPass = "pwd";

    const primary = replSet.getPrimary();
    const admin = primary.getDB("admin");
    admin.createUser({user: kUser, pwd: kPass, roles: ["root"]});

    authenticateConnection(primary);
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: true}));

    // Ensure that running a command from a non-exempt appName is rate limited.
    primary.getDB("test").runCommand({ping: 1});  // Consume the single token.
    assert.commandFailedWithCode(
        primary.getDB("test").runCommand({ping: 1}),
        ErrorCodes.RateLimitExceeded,
        "Expected ping command to be rate limited",
    );

    // Create an exempt client to run commands from.
    const exemptClient =
        new Mongo(`mongodb://${primary.host}/?appName=${kRateLimiterExemptAppName}`);
    authenticateConnection(exemptClient);

    const initialStats = getRateLimiterStats(exemptClient);

    // Run a few writes to ensure that all replication-related commands are exempted.
    const kInserts = 5;
    for (let i = 0; i < kInserts; i++) {
        assert.commandWorked(
            exemptClient.getDB("test").runCommand({
                insert: "foo",
                documents: [{_id: i}],
                writeConcern: {w: 3},
            }),
        );
    }

    const newStats = getRateLimiterStats(exemptClient);
    jsTestLog("newStats: " + tojson(newStats));
    jsTestLog("initialStats: " + tojson(initialStats));
    // Assert that all of the commands involved in doing this insert were exempted (the inserts
    // themselves + the heartbeats + the OplogFetcher commands).
    assert.gt(newStats.exemptedAdmissions - initialStats.exemptedAdmissions, kInserts);

    // Exempt the shell via driver name and observe that we can run a few commands.
    assert.commandWorked(
        exemptClient.adminCommand({
            setParameter: 1,
            ingressRequestRateLimiterApplicationExemptions: {
                appNames: ["OplogFetcher", "MongoDB Internal Client", "NetworkInterfaceTL-Repl"],
            },
        }),
    );

    // Run a few more writes to ensure that all replication-related commands are exempted.
    for (let i = 0; i < kInserts; i++) {
        assert.commandWorked(
            primary.getDB("test").runCommand({
                insert: "bar",
                documents: [{_id: i + kInserts}],
                writeConcern: {w: 3},
            }),
        );
    }

    const finalStats = getRateLimiterStats(primary);

    jsTestLog("finalStats: " + tojson(finalStats));
    jsTestLog("previousStats: " + tojson(newStats));
    // We've admitted at least a few more exempted operations (the shell commands + heartbeats),
    assert.gt(finalStats.exemptedAdmissions - newStats.exemptedAdmissions, kInserts);

    replSet.stopSet();
};

testGetSetAppExemptions();
testAppExemptionsWorkInReplSet();
