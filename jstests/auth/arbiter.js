// Certain commands should be run-able from arbiters under localhost, but not from
// any other nodes in the replset.
// @tags: [requires_replication]

(function() {

const name = "arbiter_localhost_test";
const key = "jstests/libs/key1";
const replTest = new ReplSetTest({name: name, nodes: 2, keyFile: key});
const nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate({
    _id: name,
    members: [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1], arbiterOnly: true}],
});

const primary = replTest.nodes[0];
const arbiter = replTest.nodes[1];

const testCases = [
    {
        command: {getCmdLineOpts: 1},
        expectedPrimaryCode: ErrorCodes.Unauthorized,
        expectedArbiterCode: ErrorCodes.OK,
    },
    {
        command: {getParameter: 1, logLevel: 1},
        expectedPrimaryCode: ErrorCodes.Unauthorized,
        expectedArbiterCode: ErrorCodes.OK,
    },
    {
        command: {serverStatus: 1},
        expectedPrimaryCode: ErrorCodes.Unauthorized,
        expectedArbiterCode: ErrorCodes.OK,
    },
    {
        command: {
            ping: 1,
            "$clusterTime": {
                clusterTime: Timestamp(1, 1),
                signature: {hash: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAA="), keyId: NumberLong(0)}
            }
        },
        expectedPrimaryCode: ErrorCodes.OK,
        expectedArbiterCode: ErrorCodes.OK,
    },
    {
        command: {
            isMaster: 1,
            "$clusterTime": {
                clusterTime: Timestamp(1, 1),
                signature: {hash: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAA="), keyId: NumberLong(0)}
            }
        },
        expectedPrimaryCode: ErrorCodes.OK,
        expectedArbiterCode: ErrorCodes.OK,
    },
];

function _runTestCommandOnConn(conn, command, expectedCode) {
    if (expectedCode) {
        assert.commandFailedWithCode(conn.adminCommand(command), expectedCode);
    } else {
        assert.commandWorked(conn.adminCommand(command));
    }
}

for (var testCase of testCases) {
    _runTestCommandOnConn(primary, testCase.command, testCase.expectedPrimaryCode);
    _runTestCommandOnConn(arbiter, testCase.command, testCase.expectedArbiterCode);
}

replTest.stopSet();
})();
