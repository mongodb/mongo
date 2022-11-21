// check notablescan mode
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
//   # Server parameters are stored in-memory only so are not transferred onto the recipient. This
//   # test sets the server parameter "notablescan" to force the node to not execute queries that
//   # require a collection scan and return an error.
//   tenant_migration_incompatible,
// ]

t = db.test_notablescan;
t.drop();

try {
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: true}));
    // commented lines are SERVER-2222
    if (0) {  // SERVER-2222
        assert.throws(function() {
            t.find({a: 1}).toArray();
        });
    }
    t.save({a: 1});
    assert.throws(function() {
        t.count({a: 1});
    });
    if (0) {
        assert.throws(function() {
            t.find({}).toArray();
        });
    }
    assert.eq(1, t.find({}).itcount());  // SERVER-274

    let err = assert.throws(function() {
        t.find({a: 1}).toArray();
    });
    assert.includes(err.toString(), "No indexed plans available, and running with 'notablescan'");

    err = assert.throws(function() {
        t.find({a: 1}).hint({$natural: 1}).toArray();
    });
    assert.includes(err.toString(), "$natural");
    assert.includes(err.toString(), "notablescan");

    t.createIndex({a: 1});
    assert.eq(0, t.find({a: 1, b: 1}).itcount());
    assert.eq(1, t.find({a: 1, b: null}).itcount());

    // SERVER-4327
    assert.eq(0, t.find({a: {$in: []}}).itcount());
    assert.eq(0, t.find({a: {$in: []}, b: 0}).itcount());
} finally {
    // We assume notablescan was false before this test started and restore that
    // expected value.
    assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: false}));
}
