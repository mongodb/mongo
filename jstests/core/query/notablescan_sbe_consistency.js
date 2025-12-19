// @tags: [
//   # The test runs commands that are not allowed with security token: setParameter.
//   not_allowed_with_signed_security_token,
//   assumes_against_mongod_not_mongos,
//   # This test attempts to perform read operations after having enabled the notablescan server
//   # parameter. The former operations may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
//   # Test relies on creating a collection without an index, so notablescan leads to a failure.
//   exclude_from_timeseries_crud_passthrough,
//   requires_fcv_83,
// ]

// Populate foo and foo2 collections.
db.foo.drop();
db.foo2.drop();
db.foo.createIndex({a: 1});
db.foo.insert({a: 1});
db.foo2.insert({b: 1});

const prevnotablescan = assert.commandWorked(db.adminCommand({setParameter: 1, notablescan: 1})).was;

function runWithHint(hint) {
    // Perform a $lookup which would require a table scan of foo2, as it does not have a suitable
    // index.
    let cmd = {
        aggregate: "foo",
        pipeline: [{$match: {a: 1}}, {$lookup: {from: "foo2", as: "res", localField: "a", foreignField: "b"}}],
        cursor: {},
    };
    if (hint != undefined) {
        cmd.hint = hint;
    }
    const res = db.runCommand(cmd);
    // As this query requires a table scan to be satisfied, and notablescan is set,
    // this should always fail, regardless of natural hints.
    // I.e., natural hint should not be allowed to override the notablescan parameter.
    // TODO SERVER-110051: This behaviour may change, to allow natural hint to override
    // notablescan, which would be consistent with QuerySettings allowedIndexes
    assert.commandFailedWithCode(res, [ErrorCodes.NoQueryExecutionPlans]);
}

const naturalHints = [undefined, {"$natural": []}, {"$natural": 1}, {"$natural": -1}, {"$natural": [1, -1]}];

function runCmds() {
    naturalHints.forEach(runWithHint);
}

const prevQueryEngine = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeRestricted"}),
).was;
runCmds();

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
runCmds();

assert.commandWorked(db.adminCommand({setParameter: 1, notablescan: prevnotablescan}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: prevQueryEngine}));
