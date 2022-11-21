// check notablescan mode for capped collection / tailable cursor
//
// The test runs commands that are not allowed with security token: setParameter.
// @tags: [
//   not_allowed_with_security_token,
//   assumes_against_mongod_not_mongos,
//   # This test attempts to perform read operations after having enabled the notablescan server
//   # parameter. The former operations may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
//   requires_capped,
//   # Server parameters are stored in-memory only so are not transferred onto the recipient. This
//   # test sets the server parameter "notablescan" to force the node to not execute queries that
//   # require a collection scan and return an error.
//   tenant_migration_incompatible,
// ]

t = db.test_notablescan_capped;
t.drop();
assert.commandWorked(db.createCollection(t.getName(), {capped: true, size: 100}));

try {
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: true}));

    err = assert.throws(function() {
        t.find({a: 1}).tailable(true).next();
    });
    assert.includes(err.toString(), "tailable");
    assert.includes(err.toString(), "notablescan");

} finally {
    // We assume notablescan was false before this test started and restore that
    // expected value.
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: false}));
}
