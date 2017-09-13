// checks that operations do not create a database

(function() {
    "use strict";
    var adminDB = db.getSiblingDB("admin");
    var noDB = function(db) {
        var dbName = db.getName();
        var dbsRes = assert.commandWorked(adminDB.runCommand("listDatabases"));
        dbsRes.databases.forEach(function(e) {
            assert.neq(
                dbName, e.name, "Found db which shouldn't exist:" + dbName + "; " + tojson(dbsRes));
        });
    };
    var mydb = db.getSiblingDB("neverCreated");
    mydb.dropDatabase();
    noDB(mydb);

    var coll = mydb.fake;

    // force:true is for replset passthroughs
    assert.commandFailed(coll.runCommand("compact", {force: true}));
    noDB(mydb);
    assert.writeOK(coll.insert({}));
    mydb.dropDatabase();

    assert.commandFailed(coll.runCommand("dropIndexes"));
    noDB(mydb);
    assert.writeOK(coll.insert({}));
    mydb.dropDatabase();

    assert.commandFailed(coll.runCommand("collMod", {expireAfterSeconds: 1}));
    noDB(mydb);
    assert.writeOK(coll.insert({}));
    mydb.dropDatabase();
}());