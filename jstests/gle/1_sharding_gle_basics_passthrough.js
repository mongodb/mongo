//
// Tests basic mongos GLE behavior
//

(function() {
     "use strict"

     var passST = new ShardingTest({ name : "passST", shards : 2, mongos : 1 });
     var passMongos = passST.s0;
     assert.commandWorked(passMongos.getDB("admin").runCommand({ enableSharding : "testSharded" }));

     // Remember the global 'db' var
     var lastDB = db;

     var coreTests = listFiles("jstests/gle/core");

     var testsToSkip = new RegExp('[\\/\\\\](' +
                                  'error1|' + // getPrevError not supported in sharding
                                  'remove5|' +
                                  'unique2|' +
                                  'update4' +
                                  ')\.js$');

     coreTests.forEach(
         function(file) {

             // Reset global 'db' var
             db = passMongos.getDB("testBasicMongosGLE");

             if (testsToSkip.test(file.name)) {
                 print(" !!!!!!!!!!!!!!! skipping test " + file.name);
                 return;
             }

             print(" *******************************************");
             print("         Test : " + file.name + " ...");


             var testTime = Date.timeFunc( function() { load(file.name); }, 1);
             print("                " + testTime + "ms");
         });

     print("Tests completed.");

     // Restore 'db' var
     db = lastDB;
     passST.stop();

}());