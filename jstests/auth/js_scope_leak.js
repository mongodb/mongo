/**
 * @tags: [
 *   requires_scripting
 * ]
 */
// Test for SERVER-9129
// Verify global scope data does not persist past logout or auth.
// NOTE: Each test case covers 3 state transitions:
//          no auth       -> auth user 'a'
//          auth user 'a' -> auth user 'b'
//          auth user 'b' -> logout
//
//       These transitions are tested for $where and MapReduce.

const conn = MongoRunner.runMongod();
const test = conn.getDB("test");

// insert a single document and add two test users
assert.writeOK(test.foo.insert({a: 1}));
assert.eq(1, test.foo.findOne().a);
test.createUser({user: "a", pwd: "a", roles: jsTest.basicUserRoles});
test.createUser({user: "b", pwd: "b", roles: jsTest.basicUserRoles});

function missingOrEquals(string) {
    return (
        "function() { " +
        "var global = function(){return this;}.call();" +
        // Uncomment the next line when debugging.
        // + 'print(global.hasOwnProperty("someGlobal") ? someGlobal : "MISSING" );'
        'return !global.hasOwnProperty("someGlobal")' +
        '    || someGlobal == unescape("' +
        escape(string) +
        '");' +
        "}()"
    );
}

// test $where
function testWhere() {
    // set the global variable 'someGlobal' before authenticating
    test.foo.findOne({$where: 'someGlobal = "noUsers";'});

    // test new user auth causes scope to be cleared
    assert(test.auth("a", "a"));
    assert.eq(1, test.foo.count({$where: "return " + missingOrEquals("a")}), "$where: Auth user 'a");

    // test auth as another user causes scope to be cleared
    test.foo.findOne({$where: 'someGlobal = "a";'});
    test.logout();

    test.auth("b", "b");
    assert(test.foo.count({$where: "return " + missingOrEquals("a&b")}), "$where: Auth user 'b'");
    // test user logout causes scope to be cleared
    test.foo.findOne({$where: 'someGlobal = "a&b";'});
    test.logout();

    assert(test.foo.count({$where: "return " + missingOrEquals("noUsers")}), "$where: log out");
}
testWhere();
testWhere();

function testMapReduce() {
    function mapSet(string) {
        return Function('someGlobal = "' + string + '"');
    }

    function mapGet(string) {
        return Function("assert(" + missingOrEquals(string) + ")");
    }

    function reduce(k, v) {
        // Do nothing
    }

    function setGlobalInMap(string) {
        test.foo.mapReduce(mapSet(string), reduce, {out: {inline: 1}});
    }

    function getGlobalFromMap(string) {
        test.foo.mapReduce(mapGet(string), reduce, {out: {inline: 1}});
    }

    // set the global variable 'someGlobal' before authenticating
    setGlobalInMap("noUsers");

    // test new user auth causes scope to be cleared
    assert(test.auth("a", "a"));
    assert.doesNotThrow(
        function () {
            getGlobalFromMap("a");
        },
        [],
        "M/R: Auth user 'a'",
    );

    // test auth as another user causes scope to be cleared
    setGlobalInMap("a");
    test.logout();
    assert(test.auth("b", "b"));
    assert.doesNotThrow(
        function () {
            getGlobalFromMap("a&b");
        },
        [],
        "M/R: Auth user 'b'",
    );

    // test user logout causes scope to be cleared
    setGlobalInMap("a&b");
    test.logout();
    assert.doesNotThrow(
        function () {
            getGlobalFromMap("noUsers");
        },
        [],
        "M/R: Log out",
    );
}
testMapReduce();
testMapReduce();

MongoRunner.stopMongod(conn);
