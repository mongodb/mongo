// Verify that QuerySettings is able to override notablescan, and permit a query to use a collection
// scan.
//
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
// ]

import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let db = rst.getPrimary().getDB(jsTestName());

// Populate foo and foo2 collections.
db.foo.drop();
db.foo2.drop();
db.foo.createIndex({a: 1});
db.foo.insert({a: 1});
db.foo2.insert({b: 1});

assert.commandWorked(db.adminCommand({setParameter: 1, notablescan: true}));

const qsNaturalHints = [[{"$natural": 1}], [{"$natural": -1}], [{"$natural": 1}, {"$natural": -1}]];

// Ensure Query Settings can override notablescan, consistently in SBE and Classic.
for (const internalQueryFrameworkControl of ["trySbeRestricted", "forceClassicEngine"]) {
    const prevIQFC = assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl})).was;

    const qsutils = new QuerySettingsUtils(db);

    // Perform a $lookup which would require a table scan of foo2, as it does not have a
    // suitable index.
    const cmd = {
        aggregate: "foo",
        pipeline: [{$match: {a: 1}}, {$lookup: {from: "foo2", as: "res", localField: "a", foreignField: "b"}}],
        cursor: {},
    };

    // As this query requires a table scan to be satisfied, and notablescan is set,
    // without query settings to override this, the command should fail
    assert.commandFailedWithCode(db.runCommand(cmd), [ErrorCodes.NoQueryExecutionPlans]);

    // Now try with query settings specifically allowing table scans of the foreign collection.
    for (const allowedIndexes of qsNaturalHints) {
        const ns = {db: db.getName(), coll: "foo2"};
        qsutils.withQuerySettings({...cmd, $db: db.getName()}, {indexHints: [{ns, allowedIndexes}]}, () => {
            assert.commandWorked(db.runCommand(cmd));
        });
    }

    // Confirm that settings for the main collection don't allow table scans of the _foreign_
    // collection.
    for (const allowedIndexes of qsNaturalHints) {
        // Namespace for the _main_ collection.
        const ns = {db: db.getName(), coll: "foo"};
        qsutils.withQuerySettings({...cmd, $db: db.getName()}, {indexHints: [{ns, allowedIndexes}]}, () => {
            assert.commandFailedWithCode(db.runCommand(cmd), [ErrorCodes.NoQueryExecutionPlans]);
        });
    }

    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: prevIQFC}));
}

rst.stopSet();
