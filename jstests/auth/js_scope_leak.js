// Test for SERVER-9129
// Verify global scope data does not persist past logout or auth.
// NOTE: Each test case covers 3 state transitions:
//          no auth       -> auth user 'a'
//          auth user 'a' -> auth user 'b'
//          auth user 'b' -> logout
//
//       These transitions are tested for dbEval, $where, MapReduce and $group

var conn = MongoRunner.runMongod({smallfiles: ""});
var test = conn.getDB("test");

// insert a single document and add two test users
test.foo.insert({a: 1});
assert.eq(1, test.foo.findOne().a);
test.createUser({user: 'a', pwd: 'a', roles: jsTest.basicUserRoles});
test.createUser({user: 'b', pwd: 'b', roles: jsTest.basicUserRoles});

function missingOrEquals(string) {
    return 'function() { ' +
        'var global = function(){return this;}.call();'
        // Uncomment the next line when debugging.
        // + 'print(global.hasOwnProperty("someGlobal") ? someGlobal : "MISSING" );'
        + 'return !global.hasOwnProperty("someGlobal")' +
        '    || someGlobal == unescape("' + escape(string) + '");' +
        '}()';
}

function testDbEval() {
    // set the global variable 'someGlobal' before authenticating
    test.eval('someGlobal = "noUsers";');

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert(test.eval('return ' + missingOrEquals('a')), "dbEval: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    test.eval('someGlobal = "a";');
    test.auth('b', 'b');
    assert(test.eval('return ' + missingOrEquals('a&b')), "dbEval: Auth user 'b'");

    // test user logout causes scope to be cleared
    test.eval('someGlobal = "a&b";');
    test.logout();
    assert(test.eval('return ' + missingOrEquals('noUsers')), "dbEval: log out");
}
testDbEval();
testDbEval();

// test $where
function testWhere() {
    // set the global variable 'someGlobal' before authenticating
    test.foo.findOne({$where: 'someGlobal = "noUsers";'});

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.eq(
        1, test.foo.count({$where: 'return ' + missingOrEquals('a')}), "$where: Auth user 'a");

    // test auth as another user causes scope to be cleared
    test.foo.findOne({$where: 'someGlobal = "a";'});
    test.auth('b', 'b');
    assert(test.foo.count({$where: 'return ' + missingOrEquals('a&b')}), "$where: Auth user 'b'");
    // test user logout causes scope to be cleared
    test.foo.findOne({$where: 'someGlobal = "a&b";'});
    test.logout();
    assert(test.foo.count({$where: 'return ' + missingOrEquals('noUsers')}), "$where: log out");
}
testWhere();
testWhere();

function testMapReduce() {
    var mapSet = function(string) {
        return Function('someGlobal = "' + string + '"');
    };
    var mapGet = function(string) {
        return Function('assert(' + missingOrEquals(string) + ')');
    };
    var reduce = function(k, v) {};
    var setGlobalInMap = function(string) {
        test.foo.mapReduce(mapSet(string), reduce, {out: {inline: 1}});
    };
    var getGlobalFromMap = function(string) {
        test.foo.mapReduce(mapGet(string), reduce, {out: {inline: 1}});
    };

    // set the global variable 'someGlobal' before authenticating
    setGlobalInMap('noUsers');

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.doesNotThrow(function() {
        getGlobalFromMap('a');
    }, [], "M/R: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    setGlobalInMap('a');
    test.auth('b', 'b');
    assert.doesNotThrow(function() {
        getGlobalFromMap('a&b');
    }, [], "M/R: Auth user 'b'");

    // test user logout causes scope to be cleared
    setGlobalInMap('a&b');
    test.logout();
    assert.doesNotThrow(function() {
        getGlobalFromMap('noUsers');
    }, [], "M/R: Log out");
}
testMapReduce();
testMapReduce();

function testGroup() {
    var setGlobalInGroup = function(string) {
        return test.foo.group({
            key: 'a',
            reduce: Function('doc1', 'agg', 'someGlobal = "' + string + '"'),
            initial: {}
        });
    };
    var getGlobalFromGroup = function(string) {
        return test.foo.group({
            key: 'a',
            reduce: Function('doc1', 'agg', 'assert(' + missingOrEquals(string) + ')'),
            initial: {}
        });
    };

    // set the global variable 'someGlobal' before authenticating
    setGlobalInGroup('noUsers');

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.doesNotThrow(getGlobalFromGroup, ['a'], "Group: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    setGlobalInGroup('a');
    test.auth('b', 'b');
    assert.doesNotThrow(getGlobalFromGroup, ['a&b'], "Group: Auth user 'b'");

    // test user logout causes scope to be cleared
    setGlobalInGroup('a&b');
    test.logout();
    assert.doesNotThrow(getGlobalFromGroup, ['noUsers'], "Group: Log out");
}
testGroup();
testGroup();
