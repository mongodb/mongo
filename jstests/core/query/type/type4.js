// Tests for SERVER-20080
//
// Verify that various types cannot be invoked as constructors

let t = db.jstests_type4;
t.drop();
t.insert({});
t.insert({});
t.insert({});

assert.throws(
    function () {
        new _rand()();
    },
    [],
    "invoke constructor on natively injected function",
);

assert.throws(
    function () {
        let doc = db.test.findOne();
        new doc();
    },
    [],
    "invoke constructor on BSON",
);

assert.throws(
    function () {
        let cursor = t.find();
        cursor.next();

        new cursor._cursor._cursorHandle();
    },
    [],
    "invoke constructor on CursorHandle",
);
