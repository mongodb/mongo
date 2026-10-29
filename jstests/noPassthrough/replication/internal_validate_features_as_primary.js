/**
 * Tests that the internalValidateFeaturesAsPrimary server parameter
 * and the deprecated alias internalValidateFeaturesAsMaster both work.
 * @tags: [
 * ]
 */
// internalValidateFeaturesAsPrimary can be set via startup parameter.
let conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsPrimary=0"});
assert.neq(null, conn, "mongod was unable to start up");
let res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsPrimary: 1});
assert.commandWorked(res);
assert.eq(res.internalValidateFeaturesAsPrimary, false);

// Even though we set internalValidateFeaturesAsPrimary, verify that calling
// getParameter with the deprecated alias internalValidateFeaturesAsMaster works
// and uses the value we set for internalValidateFeaturesAsPrimary.
res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
assert.commandWorked(res);
assert.eq(res.internalValidateFeaturesAsMaster, false);

// Use of deprecated parameter shows deprecation message.
let joinShell = startParallelShell(
    "db.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});",
    conn.port,
);
joinShell();
assert(
    rawMongoProgramOutput("Use of deprecated server parameter name").match(
        '"attr":{"deprecatedName":"internalValidateFeaturesAsMaster"',
    ),
);
MongoRunner.stopMongod(conn);

// internalValidateFeaturesAsMaster can be set via startup parameter.
conn = MongoRunner.runMongod({setParameter: "internalValidateFeaturesAsMaster=1"});
assert.neq(null, conn, "mongod was unable to start up");
res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsMaster: 1});
assert.commandWorked(res);
assert.eq(res.internalValidateFeaturesAsMaster, true);

// Verify that calling getParameter with internalValidateFeaturesAsPrimary
// uses the value we set for internalValidateFeaturesAsMaster.
res = conn.adminCommand({getParameter: 1, internalValidateFeaturesAsPrimary: 1});
assert.commandWorked(res);
assert.eq(res.internalValidateFeaturesAsPrimary, true);
MongoRunner.stopMongod(conn);

// internalValidateFeaturesAsPrimary cannot be set with --replSet.
// Correct error message is logged based on parameter name.
function assertStartupFailsWithReplSet(paramName, paramValue) {
    const otherName =
        paramName === "internalValidateFeaturesAsPrimary"
            ? "internalValidateFeaturesAsMaster"
            : "internalValidateFeaturesAsPrimary";
    clearRawMongoProgramOutput();
    assert.throws(
        () =>
            MongoRunner.runMongod({
                replSet: "replSetName",
                setParameter: `${paramName}=${paramValue}`,
            }),
        [],
        "mongod was unexpectedly able to start up",
    );
    let output;
    assert.soon(() => {
        output = rawMongoProgramOutput("Cannot specify both .* and replication.replSet");
        return output.match(`Cannot specify both ${paramName} and replication.replSet`);
    }, `Expected startup error message mentioning ${paramName}`);
    assert(!output.match(`Cannot specify both ${otherName} and replication.replSet`));
}

assertStartupFailsWithReplSet("internalValidateFeaturesAsPrimary", 0);
assertStartupFailsWithReplSet("internalValidateFeaturesAsPrimary", 1);
assertStartupFailsWithReplSet("internalValidateFeaturesAsMaster", 0);

// internalValidateFeaturesAsPrimary cannot be set via runtime parameter.
conn = MongoRunner.runMongod({});
assert.commandFailed(conn.adminCommand({setParameter: 1, internalValidateFeaturesAsPrimary: true}));
assert.commandFailed(
    conn.adminCommand({setParameter: 1, internalValidateFeaturesAsPrimary: false}),
);
MongoRunner.stopMongod(conn);
