
// Test db.loadServerScripts()


jsTest.log("testing db.loadServerScripts()");
var x;

// assert._debug = true;

// clear out any data from old tests
db.system.js.remove();

x = db.system.js.findOne();
assert.isnull(x, "Test for empty collection");

// User functions should not be defined yet
assert.eq( typeof myfunc, "undefined", "Checking that myfunc() is undefined" );
assert.eq( typeof myfunc2, "undefined", "Checking that myfunc2() is undefined" );

// Insert a function in the context of this process: make sure it's in the collection
db.system.js.insert( { _id: "myfunc", "value": function(){ return "myfunc"; } } );
x = db.system.js.count();
assert.eq( x, 1, "Should now be one function in the system.js collection");

// Load that function
db.loadServerScripts();
assert.eq( typeof myfunc, "function", "Checking that myfunc() loaded correctly" );

// Make sure it works
x = myfunc();
assert.eq(x, "myfunc", "Checking that myfunc() returns the correct value");

// Insert value into collection from another process
var coproc = startParallelShell(
    ' db.system.js.insert( {_id: "myfunc2", "value": function(){ return "myfunc2"; } } ); '
                );
// wait for results
coproc();

// Make sure the collection's been updated
x = db.system.js.count();
assert.eq( x, 2, "Should now be two functions in the system.js collection");


// Load the new functions: test them as above
db.loadServerScripts();
assert.eq( typeof myfunc2, "function", "Checking that myfunc2() loaded correctly" );
x = myfunc2();
assert.eq(x, "myfunc2", "Checking that myfunc2() returns the correct value");

/* 
 * This doesn't work, since exceptions in the subshell aren't propagated to the
 *   main shell
 *
 * // Test that a new shell doesn't load the existing functions by default
 * coproc = startParallelShell(
 *     // Make sure the functions are not loaded by default
 *     ' assert.eq( typeof myfunc, "undefined", "myfunc() is undefined in new process" ); ' + 
 *     ' assert.eq( typeof myfunc2, "undefined", "myfunc2() is undefined in new process" ); ' + 
 *     // Make sure they load properly there
 *     ' db.loadServerScripts();' + 
 *     ' x = myfunc2(); ' + 
 *     ' assert.eq(x, "myfunc2", "myfunc2() returns the correct value"); '
 *                 );
 * // wait for results
 * coproc();
 */

jsTest.log("completed test of db.loadServerScripts()");

