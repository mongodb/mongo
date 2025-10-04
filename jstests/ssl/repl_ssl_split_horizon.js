// Create a temporary host file that creates two aliases for localhost that are in the
// splithorizon certificate.
// The aliases are 'splithorizon1' and 'splithorizon2'
import {ReplSetTest} from "jstests/libs/replsettest.js";

const hostsFile = MongoRunner.dataPath + "split-horizon-hosts";
writeFile(hostsFile, "splithorizon1 localhost\nsplithorizon2 localhost\n");

// Check if HOSTALIASES works on this system (Will not work on Windows or OSX and may not work
// on Linux)
try {
    var rc = runMongoProgram("env", "HOSTALIASES=" + hostsFile, "getent", "hosts", "splithorizon1");
} catch (e) {
    jsTestLog(
        `Failed the check for HOSTALIASES support using env, we are probably on a non-GNU platform. Skipping this test.`,
    );
    removeFile(hostsFile);
    quit();
}

if (rc != 0) {
    removeFile(hostsFile);

    // Check glibc version to figure out of HOSTALIASES will work as expected
    clearRawMongoProgramOutput();
    rc = runProgram("getconf", "GNU_LIBC_VERSION");
    if (rc != 0) {
        jsTestLog(`Failed the check for GLIBC version, we are probably on a non-GNU platform. Skipping this test.`);
        quit();
    }

    // Output is of the format: 'glibc x.yz'
    let output = rawMongoProgramOutput(".*");
    clearRawMongoProgramOutput();

    jsTestLog(`getconf GNU_LIBC_VERSION\n${output}`);

    let fields = output.split(" ");
    let glibc_version = parseFloat(fields[2]);

    rc = runProgram("cat", "/etc/os-release");
    if (rc != 0) {
        jsTestLog(`Failed the check for /etc/os-release, we are probably not on a *nix. Skipping this test.`);
        quit();
    }

    let osRelease = rawMongoProgramOutput(".*");
    clearRawMongoProgramOutput();

    jsTestLog(`cat /etc/os-release\n${osRelease}`);

    let suzeMatch = osRelease.match(/ID="?sles"?/);

    // Fail this test if we are on GLIBC >= 2.2 and HOSTALIASES still doesn't work
    if (glibc_version < 2.2) {
        jsTestLog(
            `HOSTALIASES does not seem to work as expected on this system. GLIBC
                version is ${glibc_version}, skipping this test.`,
        );
        quit();
    } else if (suzeMatch) {
        jsTestLog(
            `HOSTALIASES does not seem to work as expected but we detected SLES. GLIBC
                version is ${glibc_version}, skipping this test.`,
        );
        quit();
    }

    assert(
        false,
        `HOSTALIASES does not seem to work as expected on this system. GLIBC
            version is ${glibc_version}`,
    );
}

let replTest = new ReplSetTest({
    name: "splitHorizontest",
    nodes: 2,
    nodeOptions: {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: "jstests/libs/splithorizon-server.pem",
        setParameter: {tlsUseSystemCA: true},
    },
    host: "localhost",
    useHostName: false,
});

replTest.startSet({
    env: {
        SSL_CERT_FILE: "jstests/libs/ca.pem",
    },
});

// Create some variables needed for our horizons, we're replacing localhost with the horizon
// name, leaving the port the same (so we can connect)
let node0 = replTest.nodeList()[0];
let node1 = replTest.nodeList()[1];
let node0localHostname = node0;
let node1localHostname = node1;
let node0horizonHostname = node0.replace("localhost", "splithorizon1");
let node1horizonHostname = node1.replace("localhost", "splithorizon1");
let node0horizonMissingHostname = node0.replace("localhost", "splithorizon2");
let node1horizonMissingHostname = node1.replace("localhost", "splithorizon2");

let config = replTest.getReplSetConfig();
config.members[0].horizons = {};
config.members[0].horizons.horizon_name = node0horizonHostname;
config.members[1].horizons = {};
config.members[1].horizons.horizon_name = node1horizonHostname;

replTest.initiate(config);

let checkExpectedHorizon = function (url, memberIndex, expectedHostname) {
    // Run isMaster in the shell and check that we get the expected hostname back. We must use the
    // isMaster command instead of its alias hello, as the initial handshake between the shell and
    // server use isMaster.
    const assertion =
        memberIndex === "me"
            ? "assert(db.runCommand({isMaster: 1})['me'] == '" + expectedHostname + "')"
            : "assert(db.runCommand({isMaster: 1})['hosts'][" + memberIndex + "] == '" + expectedHostname + "')";

    let argv = [
        "env",
        "HOSTALIASES=" + hostsFile,
        "SSL_CERT_FILE=jstests/libs/ca.pem",
        "mongo",
        "--tls",
        "--tlsCertificateKeyFile",
        "jstests/libs/splithorizon-server.pem",
        url,
        "--eval",
        assertion,
    ];
    return runMongoProgram(...argv);
};

// Using localhost should use the default horizon
let defaultURL = `mongodb://${node0localHostname}/admin?replicaSet=${replTest.name}&ssl=true`;
jsTestLog(`URL without horizon: ${defaultURL}`);
assert.eq(checkExpectedHorizon(defaultURL, 0, node0localHostname), 0, "localhost does not return horizon");
assert.eq(checkExpectedHorizon(defaultURL, "me", node0localHostname), 0, "localhost does not return horizon");
assert.eq(checkExpectedHorizon(defaultURL, 1, node1localHostname), 0, "localhost does not return horizon");

// Using 'splithorizon1' should use that horizon
let horizonURL = `mongodb://${node0horizonHostname}/admin?replicaSet=${replTest.name}&ssl=true`;
jsTestLog(`URL with horizon: ${horizonURL}`);
assert.eq(checkExpectedHorizon(horizonURL, 0, node0horizonHostname), 0, "does not return horizon as expected");
assert.eq(checkExpectedHorizon(horizonURL, "me", node0horizonHostname), 0, "does not return horizon as expected");
assert.eq(checkExpectedHorizon(horizonURL, 1, node1horizonHostname), 0, "does not return horizon as expected");

// Using 'splithorizon2' does not have a horizon so it should return default
let horizonMissingURL = `mongodb://${node0horizonMissingHostname}/admin?replicaSet=${replTest.name}&ssl=true`;
jsTestLog(`URL with horizon: ${horizonMissingURL}`);
assert.eq(checkExpectedHorizon(horizonMissingURL, 0, node0localHostname), 0, "does not return localhost as expected");
assert.eq(
    checkExpectedHorizon(horizonMissingURL, "me", node0localHostname),
    0,
    "does not return localhost as expected",
);
assert.eq(checkExpectedHorizon(horizonMissingURL, 1, node1localHostname), 0, "does not return localhost as expected");

// Check so we can replSetReconfig to add another horizon.
// Add 2 to the config version to account for the 'newlyAdded' removal reconfig.
config.version += 2;
config.members[0].horizons.other_horizon_name = node0horizonMissingHostname;
config.members[1].horizons.other_horizon_name = node1horizonMissingHostname;

assert.adminCommandWorkedAllowingNetworkError(replTest.getPrimary(), {replSetReconfig: config});

// Using 'splithorizon2' should now return the new horizon
horizonMissingURL = `mongodb://${node0horizonMissingHostname}/admin?replicaSet=${replTest.name}&ssl=true`;
jsTestLog(`URL with horizon: ${horizonMissingURL}`);
assert.eq(
    checkExpectedHorizon(horizonMissingURL, 0, node0horizonMissingHostname),
    0,
    "does not return horizon as expected",
);
assert.eq(
    checkExpectedHorizon(horizonMissingURL, "me", node0horizonMissingHostname),
    0,
    "does not return horizon as expected",
);
assert.eq(
    checkExpectedHorizon(horizonMissingURL, 1, node1horizonMissingHostname),
    0,
    "does not return horizon as expected",
);

// Change horizon to return a different port to connect to, so the feature can be used in a
// port-forwarding environment
let node0horizonHostnameDifferentPort = "splithorizon1:80";
let node1horizonHostnameDifferentPort = "splithorizon1:81";
config.version += 1;
config.members[0].horizons.horizon_name = node0horizonHostnameDifferentPort;
config.members[1].horizons.horizon_name = node1horizonHostnameDifferentPort;

assert.adminCommandWorkedAllowingNetworkError(replTest.getPrimary(), {replSetReconfig: config});

// Build the connection URL, do not set replicaSet as that will trigger the ReplicaSetMonitor
// which will fail as we can't actually connect now (port is wrong)
let horizonDifferentPortURL = `mongodb://${node0horizonHostname}/admin?ssl=true`;
jsTestLog(`URL with horizon using different port: ${horizonDifferentPortURL}`);
assert.eq(
    checkExpectedHorizon(horizonDifferentPortURL, 0, node0horizonHostnameDifferentPort),
    0,
    "does not return horizon as expected",
);
assert.eq(
    checkExpectedHorizon(horizonDifferentPortURL, "me", node0horizonHostnameDifferentPort),
    0,
    "does not return horizon as expected",
);
assert.eq(
    checkExpectedHorizon(horizonDifferentPortURL, 1, node1horizonHostnameDifferentPort),
    0,
    "does not return horizon as expected",
);

// Providing a config where horizons does not exist in all members is expected to fail
config.version += 1;
config.members[0].horizons.horizon_mismatch = node0.replace("localhost", "splithorizon3");
assert.commandFailed(replTest.getPrimary().adminCommand({replSetReconfig: config}));

// Providing a config where horizon hostnames are duplicated in members is expected to fail
config.version += 1;
config.members[1].horizons.horizon_mismatch = config.members[0].horizons.horizon_mismatch;
assert.commandFailed(replTest.getPrimary().adminCommand({replSetReconfig: config}));

// Two horizons with duplicated hostnames are not allowed
config.version += 1;
delete config.members[0].horizons.horizon_mismatch;
delete config.members[1].horizons.horizon_mismatch;
config.members[0].horizons.horizon_dup_hostname = config.members[0].horizons.horizon_name;
config.members[1].horizons.horizon_dup_hostname = config.members[1].horizons.horizon_name;
assert.commandFailed(replTest.getPrimary().adminCommand({replSetReconfig: config}));

replTest.stopSet();
removeFile(hostsFile);
