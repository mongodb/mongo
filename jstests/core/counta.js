// Check that count returns 0 in some exception cases.

t = db.jstests_counta;
t.drop();

for (i = 0; i < 10; ++i) {
    t.save({a: i});
}

// f() is undefined, causing an assertion
assert.throws(function() {
    t.count({
        $where: function() {
            if (this.a < 5) {
                return true;
            } else {
                f();
            }
        }
    });
});

// count must return error if collection name is absent
res = db.runCommand("count");
assert.eq(res.ok, 0);  // must not be OK
assert(res.code == 2);  // should fail with errorcode("BadValue"), not an massert
