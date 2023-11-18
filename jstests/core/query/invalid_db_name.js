// Ensures that invalid DB names are reported as write errors
//
// Can't create a collection with invalid database name
// @tags: [
//     assumes_no_implicit_collection_creation_on_get_collection
// ]
var invalidDB = db.getSiblingDB("NonExistentDB");

// This is a hack to bypass invalid database name checking by the DB constructor
invalidDB._name = "Invalid DB Name";

assert.writeError(invalidDB.coll.insert({x: 1}));

// Ensure that no database was created
var dbList = db.getSiblingDB('admin').runCommand({listDatabases: 1}).databases;
dbList.forEach(function(dbInfo) {
    assert.neq('Invalid DB Name', dbInfo.name, 'database with invalid name was created');
});