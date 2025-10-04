// check notablescan mode for capped collection / tailable cursor
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
//   requires_capped,
// ]

let t = db.test_notablescan_capped;
t.drop();
assert.commandWorked(db.createCollection(t.getName(), {capped: true, size: 100}));

try {
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: true}));

    let err = assert.throws(function () {
        t.find({a: 1}).tailable(true).next();
    });
    assert.includes(err.toString(), "tailable");
    assert.includes(err.toString(), "notablescan");
} finally {
    // We assume notablescan was false before this test started and restore that
    // expected value.
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: false}));
}
