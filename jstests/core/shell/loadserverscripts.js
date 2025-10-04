// @tags: [
//   requires_fastcount,
//   requires_non_retryable_writes,
//   uses_multiple_connections,
//   uses_parallel_shell,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   # DB.prototype.loadServerScripts does not behave as expected in module mode, and the SELinux
//   # test runner loads scripts with dynamic load.
//   no_selinux,
//   requires_system_dot_js_stored_functions,
//   # system.js stored functions only work for collections that live on the db-primary shard so
//   # we have to make sure it wont be moved anywhere by the balancer
//   assumes_balancer_off,
// ]

// Test db.loadServerScripts()

let testdb = db.getSiblingDB("loadserverscripts");
const systemJsColl = testdb.getCollection("system.js");

jsTest.log("testing db.loadServerScripts()");
let x;

// clear out any data from old tests
systemJsColl.remove({});

x = systemJsColl.findOne();
assert.isnull(x, "Test for empty collection");

// User functions should not be defined yet
assert.eq(typeof myfunc, "undefined", "Checking that myfunc() is undefined");
assert.eq(typeof myfunc2, "undefined", "Checking that myfunc2() is undefined");

// Insert a function in the context of this process: make sure it's in the collection
systemJsColl.insert({
    _id: "myfunc",
    "value": function () {
        return "myfunc";
    },
});
systemJsColl.insert({_id: "mystring", "value": "var root = this;"});
systemJsColl.insert({_id: "changeme", "value": false});

x = systemJsColl.count();
assert.eq(x, 3, "Should now be one function in the system.js collection");

// Set a global variable that will be over-written
var changeme = true;

// Load that function
testdb.loadServerScripts();
assert.eq(typeof myfunc, "function", "Checking that myfunc() loaded correctly");
assert.eq(typeof mystring, "string", "Checking that mystring round-tripped correctly");
assert.eq(changeme, false, "Checking that global var was overwritten");

// Make sure it works
// eslint-disable-next-line
x = myfunc();
assert.eq(x, "myfunc", "Checking that myfunc() returns the correct value");

// Insert value into collection from another process
let coproc = startParallelShell(
    'db.getSiblingDB("loadserverscripts").getCollection("system.js").insert' +
        '    ( {_id: "myfunc2", "value": function(){ return "myfunc2"; } } );',
);
// wait for results
coproc();

// Make sure the collection's been updated
x = systemJsColl.count();
assert.eq(x, 4, "Should now be two functions in the system.js collection");

// Load the new functions: test them as above
testdb.loadServerScripts();
assert.eq(typeof myfunc2, "function", "Checking that myfunc2() loaded correctly");
// eslint-disable-next-line
x = myfunc2();
assert.eq(x, "myfunc2", "Checking that myfunc2() returns the correct value");

jsTest.log("completed test of db.loadServerScripts()");
