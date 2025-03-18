/**
 * Test that tassert during query planning will log diagnostics about the query planner params.
 */
import {
    assertOnDiagnosticLogContents,
    queryPlannerAlwaysFails,
    runWithFailpoint
} from "jstests/libs/query/command_diagnostic_utils.js";

const hasEnterpriseModule = getBuildInfo().modules.includes("enterprise");

const dbName = "test";
const collName = jsTestName();
const otherCollName = collName + "_other";

function setup(conn) {
    const db = conn.getDB(dbName);

    // Setup base collection.
    const coll = db[collName];
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}, {sparse: true, collation: {locale: "en"}}));
    assert.commandWorked(coll.createIndex({b: 1}, {partialFilterExpression: {b: {$gte: 0}}}));
    coll.insert({a: 1, b: [1]});

    // Setup other collection
    const otherColl = db[otherCollName];
    otherColl.drop();
    assert.commandWorked(otherColl.createIndex({c: 1}, {unique: true}));
    otherColl.insert({c: 1});
    return db;
}

/**
 * Asserts that given diagnostic info is printed when a command tasserts.
 *
 * description - Description of this test case.
 * command - The command to run.
 * redact - Whether to enable log redaction before dumping diagnostic info.
 * expectedDiagnosticInfo -  List of strings that are expected to be in the diagnostic log.
 * expectedSecondaryDiagnosticInfo - List of diagnostics relating to secondary collections
 */
function runTest(
    {description, command, redact, expectedDiagnosticInfo, expectedSecondaryDiagnosticInfo}) {
    // Can't run cases that depend on log redaction without the enterprise module.
    if (!hasEnterpriseModule && redact) {
        return;
    }

    // Each test case needs to start a new mongod to clear the previous logs.
    const conn = MongoRunner.runMongod({useLogFiles: true});
    const db = setup(conn);

    if (hasEnterpriseModule) {
        assert.commandWorked(db.adminCommand({setParameter: 1, redactClientLogData: redact}));
    }

    const {failpointName, failpointOpts, errorCode} = queryPlannerAlwaysFails;
    runWithFailpoint(db, failpointName, failpointOpts, () => {
        jsTestLog("Running test case: " + description);
        assert.commandFailedWithCode(db.runCommand(command), errorCode, description);
    });

    assertOnDiagnosticLogContents({
        description: description,
        logFile: conn.fullOptions.logFile,
        expectedDiagnosticInfo: expectedDiagnosticInfo
    });

    // Only SBE fills out planner params for external collections.
    const framework =
        assert.commandWorked(db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}));
    if (framework.internalQueryFrameworkControl != "forceClassicEngine" &&
        expectedSecondaryDiagnosticInfo) {
        assertOnDiagnosticLogContents({
            description: description,
            logFile: conn.fullOptions.logFile,
            expectedDiagnosticInfo: expectedSecondaryDiagnosticInfo
        });
    }

    // We expect a non-zero exit code due to tassert triggered.
    MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});
}

const expectedLogContents = [
    "exists:",
    "isTimeseries:",
    "indexes: [",
    "{ a: 1.0 } sparse",
    "hasCollator",
    "{ b: 1.0 } multikey partial",
    "{ _id: 1 } unique",
];

// Test the find command.
runTest({
    description: "simple find",
    command: {find: collName, filter: {a: 1, b: 1}, limit: 1},
    expectedDiagnosticInfo: [...expectedLogContents],
});
runTest({
    description: "simple find with log redaction",
    command: {find: collName, filter: {a: 1, b: 1}, limit: 1},
    redact: true,
    expectedDiagnosticInfo: ["exists:", "isTimeseries:", 'indexes: [\\"###\\"'],
});

// Test the aggregate command.
runTest({
    description: "agg with a simple $match",
    command: {aggregate: collName, pipeline: [{$match: {a: 1, b: 1}}], cursor: {}},
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ]
});
runTest({
    description: "agg with multiple collections",
    command: {
        aggregate: collName,
        pipeline: [
            {$lookup: {from: "nonExistentColl", as: "res", localField: "a", foreignField: "b"}},
            {$lookup: {from: otherCollName, as: "res2", localField: "a", foreignField: "c"}}
        ],
        cursor: {}
    },
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ],
    expectedSecondaryDiagnosticInfo: [
        "nonExistentColl",
        "exists: false",
        "indexes: []",
        otherCollName,
        "{ c: 1.0 } unique",
    ],
});

// Test the count command.
runTest({
    description: "count",
    command: {count: collName, query: {a: 1, b: 1}},
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ]
});

// Test the delete command.
runTest({
    description: 'delete',
    command: {
        delete: collName,
        deletes: [{q: {a: 1, b: 1}, limit: 1}],
    },
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ]
});

// Test the update command.
runTest({
    description: 'update with simple filter',
    command: {
        update: collName,
        updates: [{q: {a: 1, b: 1}, u: {a: 2, b: 2}}],
    },
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ]
});

// Test the findAndModify command.
runTest({
    description: 'findAndModify remove',
    command: {
        findAndModify: collName,
        query: {a: 1, b: 1},
        remove: true,
    },
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ]
});

// Explain
runTest({
    description: "explain find",
    command: {explain: {find: collName, filter: {a: 1, b: 1}, limit: 1}},
    expectedDiagnosticInfo: [
        ...expectedLogContents,
    ],
});
