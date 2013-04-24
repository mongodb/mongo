// Test for SERVER-9129
// Verify global scope data does not persist past logout or auth.
// NOTE: Each test case covers 3 state transitions:
//          no auth       -> auth user 'a'
//          auth user 'a' -> auth user 'b'
//          auth user 'b' -> logout
//
//       These transitions are tested for dbEval, $where, MapReduce and $group

var conn = MongoRunner.runMongod({ auth: "", smallfiles: ""});
var test = conn.getDB("test");

// insert a single document and add two test users
test.foo.insert({a:1});
test.getLastError();
assert.eq(1, test.foo.findOne().a);
test.addUser('a', 'a');
test.addUser('b', 'b');

// test dbEval
(function() {
    // set the global variable 'someGlobal' before authenticating
    test.eval('someGlobal = 31337;');

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.throws(function() { test.eval('return someGlobal;') }, [], "dbEval: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    test.eval('someGlobal = 31337;');
    test.auth('b', 'b');
    assert.throws(function() { test.eval('return someGlobal;') }, [], "dbEval: Auth user 'b'");

    // test user logout causes scope to be cleared
    test.eval('someGlobal = 31337;');
    test.logout();
    assert.throws(function() { test.eval('return someGlobal;') }, [], "dbEval: log out");
})();

// test $where
(function() {
    // set the global variable 'someGlobal' before authenticating
    test.foo.findOne({$where:'someGlobal = 31337;'});

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.throws(function() { test.foo.findOne({$where:'return someGlobal;'}) }, [],
                  "$where: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    test.foo.findOne({$where:'someGlobal = 31337;'});
    test.auth('b', 'b');
    assert.throws(function() { test.foo.findOne({$where:'return someGlobal;'}) }, [],
                  "$where: Auth user 'b'");

    // test user logout causes scope to be cleared
    test.foo.findOne({$where:'someGlobal = 31337;'});
    test.logout();
    assert.throws(function() { test.foo.findOne({$where:'return someGlobal;'}) }, [],
                  "$where: log out");
})();


// test MapReduce
(function() {
    var mapSet = function() { someGlobal = 31337; }
    var mapGet = function() { emit(1, someGlobal); }
    var reduce = function(k, v) { }
    var setGlobalInMap = function() {
        test.foo.mapReduce(mapSet, reduce, {out:{inline:1}});
    }
    var getGlobalFromMap = function() {
        return test.foo.mapReduce(mapGet, reduce, {out:{inline:1}}).results[0].value;
    }

    // set the global variable 'someGlobal' before authenticating
    setGlobalInMap();

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.throws(function() { getGlobalFromMap(); }, [], "M/R: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    setGlobalInMap();
    test.auth('b', 'b');
    assert.throws(function() { getGlobalFromMap(); }, [], "M/R: Auth user 'b'");

    // test user logout causes scope to be cleared
    setGlobalInMap();
    test.logout();
    assert.throws(function() { getGlobalFromMap(); }, [], "M/R: Log out");
})();

// test group()
(function() {
    var setGlobalInGroup = function() {
        return test.foo.group({key: 'a',
                             reduce: function(doc1, agg) {
                                someGlobal = 31337;
                                return someGlobal;
                             },
                             initial:{}});
    }
    var getGlobalFromGroup = function() {
        return test.foo.group({key: 'a',
                             reduce: function(doc1, agg) {
                                 return someGlobal;
                             },
                             initial:{}});
    }

    // set the global variable 'someGlobal' before authenticating
    setGlobalInGroup();

    // test new user auth causes scope to be cleared
    test.auth('a', 'a');
    assert.throws(getGlobalFromGroup, [], "Group: Auth user 'a'");

    // test auth as another user causes scope to be cleared
    setGlobalInGroup();
    test.auth('b', 'b');
    assert.throws(getGlobalFromGroup, [], "Group: Auth user 'b'");

    // test user logout causes scope to be cleared
    setGlobalInGroup();
    test.logout();
    assert.throws(getGlobalFromGroup, [], "Group: Log out");
})();


