// Specific collections with system in their name should be able to be drop.
(function() {
    "use strict";

    let systemDBName = "system_DB";
    let systemDB = db.getSiblingDB(systemDBName);
    systemDB.dropDatabase();
    // Create system.js. 
    assert.writeOK(systemDB.system.js.insert({x: 1}));
  
    assert(systemDB.system.js.drop(), "couldn't drop system.js");
    // Database should now be empty.
    let res = systemDB.runCommand({listCollections: 1});
    assert.commandWorked(res);
    assert.eq(res.cursor.firstBatch.filter((entry) => entry.name != ("system.js")),
              [],
              systemDBName + " is not empty after deleting system.js");

})();
