// Check the return value of a db.eval function running a database query, and ensure the function's
// contents are logged in the profile log.

// Use a reserved database name to avoid a conflict in the parallel test suite.
var stddb = db;
var db = db.getSisterDB('evalb');

function profileCursor() {
    return db.system.profile.find({user: username + "@" + db.getName()});
}

function lastOp() {
    return profileCursor().sort({$natural: -1}).next();
}

try {
    username = 'jstests_evalb_user';
    db.dropUser(username);
    db.createUser({user: username, pwd: 'password', roles: jsTest.basicUserRoles});
    db.auth(username, 'password');

    t = db.evalb;
    t.drop();

    t.save({x: 3});

    assert.eq(3,
              db.eval(function() {
                  return db.evalb.findOne().x;
              }),
              'A');

    db.setProfilingLevel(2);

    assert.eq(3,
              db.eval(function() {
                  return db.evalb.findOne().x;
              }),
              'B');

    o = lastOp();
    assert(tojson(o).indexOf('findOne().x') > 0, 'C : ' + tojson(o));
} finally {
    db.setProfilingLevel(0);
    db = stddb;
}
