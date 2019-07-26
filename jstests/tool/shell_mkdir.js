// Test the shell's mkdir utility.

(function() {
"use strict";

var dir = MongoRunner.dataPath + "ShellMkdirTestDirectory";
removeFile(dir);

// Make a new directory
var res = mkdir(dir);
printjson(res);
assert(res);
assert(res["exists"]);
assert(res["created"]);

// Make the same directory again
res = mkdir(dir);
printjson(res);
assert(res);
assert(res["exists"]);
assert(!res["created"]);

// Check that we throw, rather than crash, on ""
// (see https://svn.boost.org/trac/boost/ticket/12495)
assert.throws(function() {
    mkdir("");
}, [], "");

removeFile(dir);

// check that other internal path functions do not crash on ""
assert(pathExists("") === false, "expected pathExists to return false on empty path");
assert(copyDbpath("", "") === undefined, "expected copyDbpath to return undefined on empty path");
assert(resetDbpath("") === undefined, "expected resetDbpath to return undefined on empty path");
}());
