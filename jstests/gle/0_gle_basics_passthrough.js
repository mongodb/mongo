//
// Tests the core GLE behavior
//

(function() {
     "use strict"

     var conn = MongoRunner.runMongod({});

     // Remember the global 'db' var
     var lastDB = db;

     var coreTests = listFiles("jstests/gle/core");

     coreTests.forEach( function(file) {

                            // Reset global 'db' var
                            db = conn.getDB("testBasicGLE");

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