//
// Tests the behavior of the shell's fluent (bulk) API under legacy opcode writes
//

(function() {
     "use strict"

     var conn = MongoRunner.runMongod({});

     // Explicitly disable write commands over this connection
     conn.useWriteCommands = function() { return false; };
     // Remember the global 'db' var
     var lastDB = db;

     // The fluent API tests are a subset of the standard suite
     var coreTests = listFiles("jstests/core");
     var fluentTests = [];

     var isFluentAPITest = function(fileName) {
         return /(^fluent_)|(^bulk_)/.test(fileName) && /\.js$/.test(fileName);
     };

     coreTests.forEach( function(file) {
                            if (isFluentAPITest(file.baseName))
                                fluentTests.push(file);
                        });

     fluentTests.forEach( function(file) {

                              // Reset global 'db' var
                              db = conn.getDB("testFluent");

                              print(" *******************************************");
                              print("         Test : " + file.name + " ...");

                              var testTime = Date.timeFunc( function() { load(file.name); }, 1);
                              print("                " + testTime + "ms");
                          });

     print("Tests completed.");

     // Restore 'db' var
     db = lastDB;
     MongoRunner.stopMongod(conn);
}());