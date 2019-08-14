// SERVER-36438 Ensure the 4.2 cloneDatabase() shell helper still successfully executes the clone
// command on a 4.0 server, now that the clone command has been removed as of 4.2.
(function() {
"use strict";
const oldVersion = "4.0";

let numDocs = 2000;

// 1kb string
let str = new Array(1000).toString();

let replsetDBName = "cloneDBreplset";
let standaloneDBName = "cloneDBstandalone";
let testColName = "foo";
let testViewName = "view";

jsTest.log("Create replica set");
let replTest = new ReplSetTest({name: "testSet", nodes: 3, nodeOptions: {binVersion: oldVersion}});
replTest.startSet();
replTest.initiate();
let master = replTest.getPrimary();
let masterDB = master.getDB(replsetDBName);
masterDB.dropDatabase();

jsTest.log("Create standalone server");
let standalone = MongoRunner.runMongod({binVersion: oldVersion});
let standaloneDB = standalone.getDB(replsetDBName);
standaloneDB.dropDatabase();

jsTest.log("Insert data into replica set");
let bulk = masterDB[testColName].initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({x: i, text: str});
}
assert.commandWorked(bulk.execute({w: 3}));

jsTest.log("Create view on replica set");
assert.commandWorked(masterDB.runCommand({create: testViewName, viewOn: testColName}));

// Make sure all writes have replicated to secondary.
replTest.awaitReplication();

jsTest.log("Clone db from replica set to standalone server");
standaloneDB.cloneDatabase(replTest.getURL());
assert.eq(numDocs,
          standaloneDB[testColName].find().itcount(),
          "cloneDatabase from replset to standalone failed (document counts do not match)");
assert.eq(numDocs,
          standaloneDB[testViewName].find().itcount(),
          "cloneDatabase from replset to standalone failed (count on view incorrect)");

jsTest.log("Clone db from replica set PRIMARY to standalone server");
standaloneDB.dropDatabase();
standaloneDB.cloneDatabase(master.host);
assert.eq(numDocs,
          standaloneDB[testColName].find().itcount(),
          "cloneDatabase from PRIMARY to standalone failed (document counts do not match)");
assert.eq(numDocs,
          standaloneDB[testViewName].find().itcount(),
          "cloneDatabase from PRIMARY to standalone failed (count on view incorrect)");

jsTest.log("Shut down replica set and standalone server");
MongoRunner.stopMongod(standalone);

replTest.stopSet();
})();
